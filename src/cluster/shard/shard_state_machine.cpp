#include "cluster/shard/shard_state_machine.h"
#include "cluster/shard/file_log_store.h"
#include "lexer/lexer.h"
#include "net/protocol.h"
#include "parser/parser.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>

namespace fs = std::filesystem;

namespace sql::cluster
{

    using nuraft::buffer;
    using nuraft::ptr;
    using nuraft::ulong;

    namespace
    {
        ptr<buffer> Bytes(const std::string &bytes)
        {
            ptr<buffer> out = buffer::alloc(bytes.size());
            out->put_raw(reinterpret_cast<const nuraft::byte *>(bytes.data()), bytes.size());
            out->pos(0);
            return out;
        }

        std::string ToString(buffer &data)
        {
            return std::string(reinterpret_cast<const char *>(data.data_begin()), data.size());
        }

        ExecutionResult Failure(const std::string &message)
        {
            ExecutionResult result;
            result.success = false;
            result.message = message;
            return result;
        }

        // The statement's type, or nullopt if it does not parse (the
        // session then reports the error itself)
        std::optional<StatementType> TypeOf(const std::string &sql)
        {
            try
            {
                Lexer lexer(sql);
                Parser parser(lexer);
                auto stmt = parser.ParseStatement();
                if (!stmt)
                    return std::nullopt;
                return stmt->GetType();
            }
            catch (const std::exception &)
            {
                return std::nullopt;
            }
        }

        ExecutionResult Run(Session &session, const std::string &sql)
        {
            try
            {
                return session.Execute(sql);
            }
            catch (const std::exception &e)
            {
                return Failure(e.what());
            }
        }

        void Must(const ExecutionResult &result, const std::string &what)
        {
            if (!result.success)
                throw std::runtime_error("shard replica cannot " + what + ": " + result.message);
        }
    } // namespace

    ShardStateMachine::ShardStateMachine(const std::string &directory, size_t snapshot_chunk)
        : dir_(directory), snapshot_chunk_(snapshot_chunk == 0 ? 1 : snapshot_chunk)
    {
        fs::create_directories(dir_ + "/snapshots");
        std::lock_guard<std::mutex> lock(mutex_);
        OpenDatabase();
        // The newest complete snapshot, if any
        ulong newest = 0;
        for (const auto &entry : fs::directory_iterator(dir_ + "/snapshots"))
        {
            const std::string name = entry.path().filename().string();
            if (entry.path().extension() == ".meta")
            {
                const ulong index = std::stoull(name.substr(0, name.find('.')));
                if (index > newest && fs::exists(SnapshotPath(index, ".db")))
                    newest = index;
            }
        }
        if (newest > 0)
        {
            std::string meta;
            if (ReadFile(SnapshotPath(newest, ".meta"), &meta))
            {
                ptr<buffer> bytes = Bytes(meta);
                last_snapshot_ = nuraft::snapshot::deserialize(*bytes);
            }
        }
    }

    ShardStateMachine::~ShardStateMachine()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        CloseDatabase();
    }

    void ShardStateMachine::OpenDatabase()
    {
        std::string error;
        db_ = Database::Open(dir_ + "/shard.db", &error);
        if (!db_)
            throw std::runtime_error("cannot open shard database: " + error);
        session_ = std::make_unique<Session>(db_.get());
        Catalog &catalog = db_->GetCatalog();
        if (catalog.GetTable(META_TABLE) == nullptr)
        {
            // A system table: SQL sent to the shard can read it, not change it
            catalog.CreateTable(META_TABLE, Schema({Column{"applied", DataType::INTEGER}}), true);
            catalog.InsertRow(catalog.GetTable(META_TABLE), Tuple({Value(0)}));
            if (!db_->Commit())
                throw std::runtime_error("cannot create " + std::string(META_TABLE));
        }
        const Table *meta = catalog.GetTable(META_TABLE);
        if (meta->GetTupleCount() != 1)
            throw std::runtime_error(std::string("shard replica: ") + META_TABLE + " must hold exactly one row");
        last_applied_ = static_cast<ulong>(meta->begin()->GetValue(0).GetAsInt());
    }

    void ShardStateMachine::RecordApplied(ulong index)
    {
        Catalog &catalog = db_->GetCatalog();
        Table *meta = catalog.GetTable(META_TABLE);
        const RID rid = meta->begin().GetRID();
        if (!catalog.UpdateRow(meta, rid, Tuple({Value(static_cast<int32_t>(index))})))
            throw std::runtime_error(std::string("shard replica cannot record the applied index in ") + META_TABLE);
    }

    void ShardStateMachine::CloseDatabase()
    {
        session_.reset();
        db_.reset();
    }

    ptr<buffer> ShardStateMachine::EncodeWrite(const std::string &sql)
    {
        return Bytes(std::string(1, static_cast<char>(Kind::WRITE)) + sql);
    }

    ptr<buffer> ShardStateMachine::EncodeBarrier() { return Bytes(std::string(1, static_cast<char>(Kind::BARRIER))); }

    ExecutionResult ShardStateMachine::DecodeResult(buffer *result)
    {
        if (result == nullptr)
            return Failure("the shard returned no result");
        net::Frame frame{net::MessageType::RESULTS, ToString(*result)};
        auto decoded = net::DecodeResults(frame);
        if (!decoded || decoded->results.size() != 1)
            return Failure("malformed result from the shard");
        return std::move(decoded->results.front());
    }

    ExecutionResult ShardStateMachine::Apply(ulong index, const std::string &sql)
    {
        // Transactions across entries are two-phase commit's job (D6)
        if (TypeOf(sql) == StatementType::TRANSACTION_STMT)
            return Failure("BEGIN, COMMIT and ROLLBACK cannot be sent to a shard directly");
        Must(session_->Execute("BEGIN;"), "start a transaction");
        // A failing statement is rolled back on its own; the transaction
        // stays open for the applied index
        ExecutionResult result = Run(*session_, sql);
        RecordApplied(index); // inside the same transaction
        Must(session_->Execute("COMMIT;"), "commit");
        return result;
    }

    ptr<buffer> ShardStateMachine::commit(const ulong log_idx, buffer &data)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string payload = ToString(data);
        ExecutionResult result;
        if (log_idx <= last_applied_)
            result = Failure("already applied"); // cannot happen: NuRaft replays after last_commit_index
        else if (!payload.empty() && payload[0] == static_cast<char>(Kind::WRITE))
            result = Apply(log_idx, payload.substr(1));
        else
        {
            // A barrier (or anything unknown): only the index moves on
            RecordApplied(log_idx);
            if (!db_->Commit())
                throw std::runtime_error("shard replica cannot commit");
            result.success = true;
        }
        last_applied_ = std::max<ulong>(last_applied_, log_idx);
        net::ResultsResponse response;
        response.results.push_back(std::move(result));
        return Bytes(net::Encode(response).payload);
    }

    ExecutionResult ShardStateMachine::Query(const std::string &sql)
    {
        const auto type = TypeOf(sql);
        if (type && *type != StatementType::SELECT && *type != StatementType::EXPLAIN_STMT)
            return Failure("only SELECT and EXPLAIN can be read from a replica");
        std::lock_guard<std::mutex> lock(mutex_);
        return Run(*session_, sql);
    }

    std::string ShardStateMachine::SnapshotPath(ulong index, const char *suffix) const
    {
        return dir_ + "/snapshots/" + std::to_string(index) + suffix;
    }

    void ShardStateMachine::DropOldSnapshots(ulong keep)
    {
        for (const auto &entry : fs::directory_iterator(dir_ + "/snapshots"))
        {
            const std::string name = entry.path().filename().string();
            const size_t dot = name.find('.');
            if (dot == 0 || dot == std::string::npos || name.find_first_not_of("0123456789") != dot)
                continue;
            if (std::stoull(name.substr(0, dot)) != keep)
                fs::remove(entry.path());
        }
    }

    void ShardStateMachine::create_snapshot(nuraft::snapshot &s, nuraft::async_result<bool>::handler_type &when_done)
    {
        bool created = false;
        ptr<std::exception> error;
        try
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // NuRaft asks right after applying s's index, before any later
            // entry: the database is exactly the state as of that index
            if (last_applied_ != s.get_last_log_idx())
                throw std::runtime_error("snapshot index " + std::to_string(s.get_last_log_idx()) +
                                         " is not the applied index " + std::to_string(last_applied_.load()));
            if (!db_->Checkpoint()) // everything in shard.db itself
                throw std::runtime_error("checkpoint failed");
            const ulong index = s.get_last_log_idx();
            fs::copy_file(dir_ + "/shard.db", SnapshotPath(index, ".db.tmp"), fs::copy_options::overwrite_existing);
            fs::rename(SnapshotPath(index, ".db.tmp"), SnapshotPath(index, ".db"));
            ptr<buffer> meta = s.serialize();
            WriteFileAtomically(SnapshotPath(index, ".meta"), ToString(*meta));
            last_snapshot_ = nuraft::snapshot::deserialize(*meta);
            DropOldSnapshots(index);
            created = true;
        }
        catch (const std::exception &e)
        {
            error = nuraft::cs_new<std::runtime_error>(e.what());
        }
        when_done(created, error);
    }

    ptr<nuraft::snapshot> ShardStateMachine::last_snapshot()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return last_snapshot_;
    }

    int ShardStateMachine::read_logical_snp_obj(nuraft::snapshot &s, void *&, ulong obj_id, ptr<buffer> &data_out,
                                                bool &is_last_obj)
    {
        const std::string path = SnapshotPath(s.get_last_log_idx(), ".db");
        std::error_code ec;
        const uintmax_t total = fs::file_size(path, ec);
        if (ec)
            return -1; // gone: NuRaft retries with a newer snapshot
        const uintmax_t offset = static_cast<uintmax_t>(obj_id) * snapshot_chunk_;
        if (offset > total)
            return -1;
        const size_t size = static_cast<size_t>(std::min<uintmax_t>(snapshot_chunk_, total - offset));
        std::string piece(size, '\0');
        std::ifstream in(path, std::ios::binary);
        in.seekg(static_cast<std::streamoff>(offset));
        if (!in.read(piece.data(), static_cast<std::streamsize>(size)))
            return -1;
        if (obj_id == 0)
        {
            // The first piece starts with the whole file's size and CRC32,
            // so the receiver can tell a complete copy from a broken one
            std::string whole;
            if (!ReadFile(path, &whole) || whole.size() != total)
                return -1;
            const uint64_t file_size = total;
            const uint32_t crc = Crc32(whole.data(), whole.size());
            std::string header(reinterpret_cast<const char *>(&file_size), sizeof(file_size));
            header.append(reinterpret_cast<const char *>(&crc), sizeof(crc));
            piece = header + piece;
        }
        data_out = Bytes(piece);
        is_last_obj = offset + size >= total;
        return 0;
    }

    void ShardStateMachine::save_logical_snp_obj(nuraft::snapshot &s, ulong &obj_id, buffer &data, bool is_first_obj,
                                                 bool is_last_obj)
    {
        const std::string part = SnapshotPath(s.get_last_log_idx(), ".db.part");
        const char *bytes = reinterpret_cast<const char *>(data.data_begin());
        size_t size = data.size();
        if (is_first_obj)
        {
            fs::remove(part);
            receiving_ok_ = size >= 12;
            if (receiving_ok_)
            {
                std::memcpy(&expected_size_, bytes, 8);
                std::memcpy(&expected_crc_, bytes + 8, 4);
                bytes += 12;
                size -= 12;
            }
        }
        if (receiving_ok_)
        {
            std::ofstream out(part, std::ios::binary | std::ios::app);
            out.write(bytes, static_cast<std::streamsize>(size));
            receiving_ok_ = static_cast<bool>(out);
        }
        if (is_last_obj)
        {
            // Install only a complete, intact copy; otherwise apply_snapshot
            // finds nothing, fails, and the leader sends the snapshot again
            std::string whole;
            if (receiving_ok_ && ReadFile(part, &whole) && whole.size() == expected_size_ &&
                Crc32(whole.data(), whole.size()) == expected_crc_)
                fs::rename(part, SnapshotPath(s.get_last_log_idx(), ".db"));
            else
                fs::remove(part);
        }
        ++obj_id;
    }

    bool ShardStateMachine::apply_snapshot(nuraft::snapshot &s)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const ulong index = s.get_last_log_idx();
        if (!fs::exists(SnapshotPath(index, ".db")))
            return false;
        CloseDatabase();
        // Replace the data with the snapshot's; its __raft_meta says index
        fs::copy_file(SnapshotPath(index, ".db"), dir_ + "/shard.db.tmp", fs::copy_options::overwrite_existing);
        fs::remove(dir_ + "/shard.db-wal");
        fs::rename(dir_ + "/shard.db.tmp", dir_ + "/shard.db");
        OpenDatabase();
        ptr<buffer> meta = s.serialize();
        WriteFileAtomically(SnapshotPath(index, ".meta"), ToString(*meta));
        last_snapshot_ = nuraft::snapshot::deserialize(*meta);
        DropOldSnapshots(index);
        return last_applied_ == index;
    }

} // namespace sql::cluster
