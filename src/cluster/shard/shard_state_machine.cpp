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
        if (catalog.GetTable(REQUESTS_TABLE) == nullptr)
        {
            Column id("id", DataType::VARCHAR, 255);
            id.primary_key = id.not_null = true; // indexed: retries are looked up by id
            catalog.CreateTable(REQUESTS_TABLE,
                                Schema({id, Column("idx", DataType::INTEGER), Column("success", DataType::BOOLEAN),
                                        Column("message", DataType::VARCHAR)}),
                                true);
            if (!db_->Commit())
                throw std::runtime_error("cannot create " + std::string(REQUESTS_TABLE));
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

    ptr<buffer> ShardStateMachine::EncodeWrite(const std::string &sql, const std::string &request_id)
    {
        if (request_id.empty() || request_id.size() > 255)
            throw std::invalid_argument("a request id has 1 to 255 characters");
        return Bytes(std::string(1, static_cast<char>(Kind::WRITE)) +
                     std::string(1, static_cast<char>(request_id.size())) + request_id + sql);
    }

    std::optional<ExecutionResult> ShardStateMachine::Recorded(const std::string &request_id)
    {
        Table *requests = db_->GetCatalog().GetTable(REQUESTS_TABLE);
        BTree *by_id = db_->GetCatalog().GetIndex(REQUESTS_TABLE, "id");
        if (!by_id)
            throw std::runtime_error(std::string(REQUESTS_TABLE) + " has no index on id");
        for (const RID &rid : by_id->Search(Value(request_id)))
        {
            Tuple row;
            if (requests->GetTuple(rid, &row))
                return ExecutionResult{row.GetValue(2).GetAsBool(), row.GetValue(3).ToString(), {}, {}};
        }
        return std::nullopt;
    }

    void ShardStateMachine::Record(ulong index, const std::string &request_id, const ExecutionResult &result)
    {
        Catalog &catalog = db_->GetCatalog();
        Table *requests = catalog.GetTable(REQUESTS_TABLE);
        catalog.InsertRow(requests, Tuple({Value(request_id), Value(static_cast<int32_t>(index)),
                                           Value(result.success), Value(result.message)}));
        // Now and then, forget requests too old to be retried
        if (index % 1024 == 0 && index > REQUEST_WINDOW)
        {
            std::vector<RID> old;
            for (auto it = requests->begin(); it != requests->end(); ++it)
                if (static_cast<ulong>(it->GetValue(1).GetAsInt()) < index - REQUEST_WINDOW)
                    old.push_back(it.GetRID());
            for (const RID &rid : old)
                catalog.DeleteRow(requests, rid);
        }
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

    ptr<buffer> ShardStateMachine::EncodeBatch(const std::string &script, const std::string &request_id)
    {
        if (request_id.empty() || request_id.size() > 255)
            throw std::invalid_argument("a request id has 1 to 255 characters");
        return Bytes(std::string(1, static_cast<char>(Kind::BATCH)) +
                     std::string(1, static_cast<char>(request_id.size())) + request_id + script);
    }

    ptr<buffer> ShardStateMachine::EncodeTxn(TxnOp op, const std::string &txid, const std::string &sql,
                                             const std::string &request_id)
    {
        if (request_id.empty() || request_id.size() > 255)
            throw std::invalid_argument("a request id has 1 to 255 characters");
        if (txid.empty() || txid.size() > 255)
            throw std::invalid_argument("a transaction id has 1 to 255 characters");
        return Bytes(std::string(1, static_cast<char>(Kind::TXN)) + std::string(1, static_cast<char>(op)) +
                     std::string(1, static_cast<char>(request_id.size())) + request_id +
                     std::string(1, static_cast<char>(txid.size())) + txid + sql);
    }

    bool ShardStateMachine::ParseTxnOp(const std::string &name, TxnOp *op)
    {
        static const std::pair<const char *, TxnOp> names[] = {{"exec", TxnOp::EXEC},       {"undo", TxnOp::UNDO},
                                                               {"prepare", TxnOp::PREPARE}, {"commit", TxnOp::COMMIT},
                                                               {"abort", TxnOp::ABORT}};
        for (const auto &[text, value] : names)
            if (name == text)
            {
                *op = value;
                return true;
            }
        return false;
    }

    std::string ShardStateMachine::OpenTransaction()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return open_txid_;
    }

    ExecutionResult ShardStateMachine::Locked() const
    {
        return Failure("shard is locked by another transaction (" + open_txid_ + ")");
    }

    void ShardStateMachine::Advance(ulong index)
    {
        RecordApplied(index);
        if (open_txid_.empty() && !db_->Commit())
            throw std::runtime_error("shard replica cannot commit");
    }

    namespace
    {
        // The wrapping transaction is ours, not the client's: report the
        // error as a single node would
        void StripRollbackNote(ExecutionResult *result)
        {
            const std::string suffix = " (statement rolled back; transaction still open)";
            if (!result->success && result->message.size() >= suffix.size() &&
                result->message.compare(result->message.size() - suffix.size(), suffix.size(), suffix) == 0)
                result->message.erase(result->message.size() - suffix.size());
        }
    } // namespace

    ExecutionResult ShardStateMachine::Apply(ulong index, const std::string &sql, const std::string &request_id,
                                             bool batch)
    {
        // A retry of a write that already went through: its first outcome
        if (auto first = Recorded(request_id))
        {
            Advance(index);
            return *first;
        }
        // Nothing else may change the data while a transaction holds it.
        // Refusing changes nothing, so a retry later is safe.
        if (!open_txid_.empty())
        {
            Advance(index);
            return Locked();
        }
        if (!batch && TypeOf(sql) == StatementType::TRANSACTION_STMT)
            return Failure("BEGIN, COMMIT and ROLLBACK cannot be sent to a shard directly");
        Must(session_->Execute("BEGIN;"), "start a transaction");
        ExecutionResult result;
        if (batch)
        {
            // All or nothing: the first failure undoes the whole batch
            for (auto &one : session_->ExecuteScript(sql, true))
            {
                result = std::move(one);
                if (!result.success)
                    break;
            }
            if (!result.success)
            {
                StripRollbackNote(&result);
                Must(session_->Execute("ROLLBACK;"), "roll back");
                Must(session_->Execute("BEGIN;"), "start a transaction");
            }
        }
        else
        {
            // A failing statement is rolled back on its own; the transaction
            // stays open for the applied index
            result = Run(*session_, sql);
            StripRollbackNote(&result);
        }
        Record(index, request_id, result); // inside the same transaction
        RecordApplied(index);
        Must(session_->Execute("COMMIT;"), "commit");
        return result;
    }

    ExecutionResult ShardStateMachine::ApplyTxn(ulong index, TxnOp op, const std::string &txid, const std::string &sql,
                                                const std::string &request_id)
    {
        if (auto first = Recorded(request_id))
        {
            Advance(index);
            return *first;
        }
        const bool mine = open_txid_ == txid;
        ExecutionResult result;
        result.success = true;
        switch (op)
        {
        case TxnOp::EXEC:
            if (!open_txid_.empty() && !mine)
                result = Locked();
            else if (prepared_)
                result = Failure("transaction " + txid + " is already prepared");
            else if (TypeOf(sql) == StatementType::TRANSACTION_STMT)
                result = Failure("BEGIN, COMMIT and ROLLBACK cannot be sent to a shard directly");
            else
            {
                if (!mine)
                {
                    Must(session_->Execute("BEGIN;"), "start a transaction");
                    open_txid_ = txid;
                    prepared_ = false;
                }
                if (!db_->CreateSavepoint(&statement_savepoint_))
                    throw std::runtime_error("shard replica cannot take a savepoint");
                has_statement_savepoint_ = true;
                result = Run(*session_, sql);
                StripRollbackNote(&result);
                Record(index, request_id, result);
            }
            break;
        case TxnOp::UNDO:
            if (!mine || prepared_ || !has_statement_savepoint_)
                result = Failure("nothing to undo in transaction " + txid);
            else
            {
                std::string error;
                if (!db_->RollbackTo(statement_savepoint_, &error))
                    throw std::runtime_error("shard replica cannot undo a statement: " + error);
                has_statement_savepoint_ = false;
                result.message = "undone";
            }
            break;
        case TxnOp::PREPARE:
            if (!mine)
                result = Failure("transaction " + txid + " is not open on this shard");
            else
            {
                prepared_ = true;
                has_statement_savepoint_ = false;
                result.message = "prepared";
            }
            break;
        case TxnOp::COMMIT:
            if (!mine)
                result = Failure("transaction " + txid + " is not open on this shard");
            else
            {
                result.message = "committed";
                Record(index, request_id, result);
                RecordApplied(index);
                open_txid_.clear();
                prepared_ = has_statement_savepoint_ = false;
                Must(session_->Execute("COMMIT;"), "commit");
                return result;
            }
            break;
        case TxnOp::ABORT:
            if (mine)
            {
                Must(session_->Execute("ROLLBACK;"), "roll back");
                open_txid_.clear();
                prepared_ = has_statement_savepoint_ = false;
            }
            result.message = "aborted";
            break;
        }
        Advance(index);
        return result;
    }

    ptr<buffer> ShardStateMachine::commit(const ulong log_idx, buffer &data)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string payload = ToString(data);
        ExecutionResult result;
        if (log_idx <= last_applied_)
            result = Failure("already applied"); // cannot happen: NuRaft replays after last_commit_index
        else if (payload.size() >= 2 && payload[0] == static_cast<char>(Kind::WRITE) &&
                 payload.size() >= 2 + static_cast<size_t>(static_cast<unsigned char>(payload[1])))
        {
            const size_t id_size = static_cast<unsigned char>(payload[1]);
            result = Apply(log_idx, payload.substr(2 + id_size), payload.substr(2, id_size), false);
        }
        else if (payload.size() >= 2 && payload[0] == static_cast<char>(Kind::BATCH) &&
                 payload.size() >= 2 + static_cast<size_t>(static_cast<unsigned char>(payload[1])))
        {
            const size_t id_size = static_cast<unsigned char>(payload[1]);
            result = Apply(log_idx, payload.substr(2 + id_size), payload.substr(2, id_size), true);
        }
        else if (payload.size() >= 4 && payload[0] == static_cast<char>(Kind::TXN))
        {
            const size_t id_size = static_cast<unsigned char>(payload[2]);
            const size_t txid_at = 3 + id_size;
            const size_t txid_size = payload.size() > txid_at ? static_cast<unsigned char>(payload[txid_at]) : 0;
            if (txid_size == 0 || payload.size() < txid_at + 1 + txid_size || static_cast<uint8_t>(payload[1]) > 4)
            {
                Advance(log_idx);
                result = Failure("malformed transaction entry");
            }
            else
                result = ApplyTxn(log_idx, static_cast<TxnOp>(payload[1]), payload.substr(txid_at + 1, txid_size),
                                  payload.substr(txid_at + 1 + txid_size), payload.substr(3, id_size));
        }
        else
        {
            // A barrier (or anything unknown): only the index moves on
            Advance(log_idx);
            result.success = true;
        }
        last_applied_ = std::max<ulong>(last_applied_, log_idx);
        net::ResultsResponse response;
        response.results.push_back(std::move(result));
        return Bytes(net::Encode(response).payload);
    }

    ExecutionResult ShardStateMachine::Query(const std::string &sql, const std::string &txid)
    {
        const auto type = TypeOf(sql);
        if (type && *type != StatementType::SELECT && *type != StatementType::EXPLAIN_STMT)
            return Failure("only SELECT and EXPLAIN can be read from a replica");
        std::lock_guard<std::mutex> lock(mutex_);
        // Uncommitted changes are visible only to their own transaction
        if (!open_txid_.empty() && open_txid_ != txid)
            return Locked();
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
            // The open transaction's changes are not committed: no snapshot
            // until it ends (replaying the log reopens it instead)
            if (!open_txid_.empty())
                throw std::runtime_error("a transaction is open");
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
        CloseDatabase(); // rolls back an open transaction
        open_txid_.clear();
        prepared_ = has_statement_savepoint_ = false;
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
