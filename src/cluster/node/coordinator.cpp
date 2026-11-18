#include "cluster/node/coordinator.h"
#include "cli/shell.h"
#include "cluster/node/script.h"
#include "cluster/node/storage_node.h"
#include "cluster/ring/hash.h"
#include "cluster/ring/placement.h"
#include "execution/evaluator.h"
#include "lexer/lexer.h"
#include "parser/parser.h"
#include "session/session.h"
#include <algorithm>
#include <chrono>
#include <random>
#include <set>
#include <sstream>

namespace sql::cluster
{

    namespace
    {
        using Clock = std::chrono::steady_clock;

        ExecutionResult Failure(const std::string &message) { return ExecutionResult{false, message, {}, {}}; }
        ExecutionResult Success(const std::string &message) { return ExecutionResult{true, message, {}, {}}; }

        std::string Quote(const std::string &text)
        {
            std::string out = "'";
            for (char c : text)
                out += c == '\'' ? std::string("''") : std::string(1, c);
            return out + "'";
        }

        std::string Text(const Value &value) { return value.IsNull() ? "" : value.ToString(); }

        // The number at the start of "N row(s) inserted." and the like
        long long Count(const std::string &message)
        {
            try
            {
                return std::stoll(message);
            }
            catch (const std::exception &)
            {
                return 0;
            }
        }

        std::string RenderValue(const Value &value)
        {
            const LiteralExpression literal(value);
            return ExpressionToSQL(&literal);
        }

        bool ContainsSubquery(const Expression *expr)
        {
            if (!expr)
                return false;
            if (expr->GetType() == ExpressionType::SUBQUERY)
                return true;
            for (const Expression *child : ExpressionChildren(expr))
                if (ContainsSubquery(child))
                    return true;
            return false;
        }

        // Failures that say nothing about the SQL: the shard was not reached,
        // or not through its leader
        bool IsUnavailable(const std::string &message)
        {
            for (const char *marker : {"not the leader", "not committed", "is stopped", "no replica of shard",
                                       "has not joined yet", "stale epoch", "bad call", "unknown call"})
                if (message.find(marker) != std::string::npos)
                    return true;
            return false;
        }

        // A shard refused: another transaction holds it (wait and retry)
        bool IsBusy(const ExecutionResult &result)
        {
            return !result.success && result.message.find("locked by another transaction") != std::string::npos;
        }

        const char *const KEPT_OPEN = " (statement rolled back; transaction still open)";

        std::unique_ptr<Statement> ParseOne(const std::string &sql, std::string *error)
        {
            try
            {
                Lexer lexer(sql);
                Parser parser(lexer);
                auto stmt = parser.ParseStatement();
                if (!parser.AtEnd())
                {
                    *error = "Expected a single statement; use ExecuteScript for several.";
                    return nullptr;
                }
                if (!stmt)
                    *error = "Failed to parse SQL statement.";
                return stmt;
            }
            catch (const std::exception &e)
            {
                *error = std::string("Parse error: ") + e.what();
                return nullptr;
            }
        }

        // What a client of a coordinator talks to: the leader's operations,
        // or, on a follower, the leader itself over the network
        class ClusterConnection : public Connection
        {
        public:
            explicit ClusterConnection(Coordinator *coordinator) : coordinator_(coordinator) {}
            ~ClusterConnection() override { coordinator_->Abandon(&txn_); }

            ExecutionResult Execute(const std::string &sql) override
            {
                // During an election there is no leader for a moment: wait
                // for the next one rather than fail at once
                const auto deadline = Clock::now() + std::chrono::milliseconds(coordinator_->ElectionWaitMs());
                for (;;)
                {
                    // A transaction lives on one leader; never let a later
                    // statement of it run elsewhere as a statement of its own
                    if (forward_ && forward_->InTransaction() &&
                        (coordinator_->IsLeader() || coordinator_->LeaderAddress() != forward_address_))
                    {
                        forward_.reset();
                        return Failure("the transaction was rolled back: the leading coordinator changed");
                    }
                    if (coordinator_->IsLeader())
                        return coordinator_->Execute(sql, &txn_);
                    if (coordinator_->Drop(&txn_)) // its shards are rolled back by the next leader
                        return Failure("the transaction was rolled back: the coordinator lost the leadership");
                    net::RemoteConnection *leader = nullptr;
                    try
                    {
                        leader = Leader();
                    }
                    catch (const std::exception &)
                    {
                        forward_.reset(); // not reached: nothing was sent
                    }
                    if (leader)
                    {
                        const bool in_transaction = leader->InTransaction();
                        try
                        {
                            return leader->Execute(sql);
                        }
                        catch (const std::exception &e)
                        {
                            forward_.reset();
                            return Failure(std::string("lost the leading coordinator (the statement's outcome is "
                                                       "unknown") +
                                           (in_transaction ? "; the transaction was rolled back" : "") +
                                           "): " + e.what());
                        }
                    }
                    if (Clock::now() >= deadline)
                        return Failure("no coordinator is leading right now; try again shortly");
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
            }

            std::vector<ExecutionResult> ExecuteScript(const std::string &sql, bool stop_on_error) override
            {
                std::vector<ExecutionResult> results;
                for (const auto &statement : SplitStatements(sql))
                {
                    results.push_back(Execute(statement));
                    if (stop_on_error && !results.back().success)
                        break;
                }
                return results;
            }

            bool InTransaction() override
            {
                if (coordinator_->InTransaction(&txn_))
                    return true;
                return !coordinator_->IsLeader() && forward_ && forward_->InTransaction();
            }

            std::string Command(const std::string &name, const std::string &argument) override
            {
                if (coordinator_->IsLeader())
                    return coordinator_->Command(name, argument);
                try
                {
                    if (auto *leader = Leader())
                        return leader->Command(name, argument);
                }
                catch (const std::exception &e)
                {
                    forward_.reset();
                    return std::string("Error: lost the leading coordinator: ") + e.what() + "\n";
                }
                return "Error: no coordinator is leading right now\n";
            }

            ExecutionResult Call(const std::string &method, const std::vector<std::string> &arguments) override
            {
                return coordinator_->Call(method, arguments); // storage nodes find the leader themselves
            }

        private:
            net::RemoteConnection *Leader()
            {
                const std::string address = coordinator_->LeaderAddress();
                if (address.empty())
                    return nullptr;
                if (!forward_ || address != forward_address_)
                {
                    const size_t colon = address.rfind(':');
                    forward_ = std::make_unique<net::RemoteConnection>(
                        address.substr(0, colon), static_cast<uint16_t>(std::stoi(address.substr(colon + 1))));
                    forward_address_ = address;
                }
                return forward_.get();
            }

            Coordinator *coordinator_;
            std::unique_ptr<net::RemoteConnection> forward_;
            std::string forward_address_;
            Coordinator::Transaction txn_;
        };
    } // namespace

    Coordinator::Coordinator(const Options &options) : options_(options) {}

    Coordinator::~Coordinator() { Stop(); }

    void Coordinator::Start()
    {
        ReplicatedShard::Options meta;
        meta.id = options_.id;
        meta.port = options_.raft_port;
        meta.directory = options_.directory + "/meta";
        meta.members = options_.peers;
        meta.heartbeat_ms = options_.heartbeat_ms;
        meta.election_timeout_ms = options_.election_timeout_ms;
        meta.snapshot_distance = options_.snapshot_distance;
        metadata_ = std::make_unique<ReplicatedShard>(meta);
        server_ = std::make_unique<Server>([this] { return std::make_unique<ClusterConnection>(this); }, false);
        server_->Start(options_.host, options_.client_port);
        loop_ = std::thread([this] { Loop(); });
    }

    void Coordinator::Stop()
    {
        if (stopping_.exchange(true))
            return;
        if (loop_.joinable())
            loop_.join();
        if (server_)
            server_->Stop();
        std::lock_guard<std::mutex> lock(op_mutex_);
        connections_.clear();
        metadata_.reset();
    }

    std::string Coordinator::LeaderAddress() const
    {
        if (!metadata_)
            return "";
        auto it = options_.client_addresses.find(metadata_->LeaderId());
        return it == options_.client_addresses.end() ? "" : it->second;
    }

    void Coordinator::Loop()
    {
        while (!stopping_)
        {
            {
                std::lock_guard<std::mutex> lock(op_mutex_);
                try
                {
                    if (IsLeader())
                    {
                        if (!was_leader_)
                            BecomeLeader();
                        Reconcile();
                    }
                    else
                        was_leader_ = false;
                }
                catch (const std::exception &)
                {
                    was_leader_ = false; // start over on the next round
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    ExecutionResult Coordinator::MetaWrite(const std::string &sql, bool batch)
    {
        // An uncertain failure (a leader change cancelled the request) is
        // retried with the same request id, which applies at most once
        const std::string id = ReplicatedShard::NewRequestId();
        const auto deadline = Clock::now() + std::chrono::milliseconds(options_.election_timeout_ms * 6 + 2000);
        for (;;)
        {
            ExecutionResult result = batch ? metadata_->WriteBatch(sql, id) : metadata_->Write(sql, id);
            if (result.success || !IsUnavailable(result.message))
                return result;
            if (!IsLeader() || Clock::now() >= deadline)
                throw std::runtime_error("metadata unavailable: " + result.message);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    ExecutionResult Coordinator::MetaRead(const std::string &sql)
    {
        ExecutionResult result = metadata_->Read(sql);
        if (!result.success && IsUnavailable(result.message))
            throw std::runtime_error("metadata unavailable: " + result.message);
        return result;
    }

    void Coordinator::BecomeLeader()
    {
        if (!MetaRead("SELECT shards FROM cluster;").success)
        {
            // The first leader ever: create the metadata tables
            for (const char *ddl : {
                     "CREATE TABLE cluster (shards INTEGER, rf INTEGER, epoch INTEGER, formed BOOLEAN);",
                     "CREATE TABLE nodes (id INTEGER PRIMARY KEY, name VARCHAR(200) UNIQUE, host VARCHAR(255), "
                     "port INTEGER, raft_base INTEGER);",
                     "CREATE TABLE replicas (shard INTEGER, position INTEGER, node INTEGER);",
                     "CREATE TABLE tables (name VARCHAR(200) PRIMARY KEY, definition VARCHAR(2000), kind "
                     "VARCHAR(20), shard_column VARCHAR(200));",
                     "CREATE TABLE ddl (seq INTEGER PRIMARY KEY, statement VARCHAR(2000));",
                     "CREATE TABLE shard_ddl (shard INTEGER PRIMARY KEY, applied INTEGER);",
                 })
                MetaWrite(ddl);
        }
        if (!MetaRead("SELECT COUNT(*) FROM txns;").success)
            MetaWrite("CREATE TABLE txns (id VARCHAR(300) PRIMARY KEY, state VARCHAR(20));");
        if (!MetaRead("SELECT COUNT(*) FROM node_state;").success)
            MetaWrite("CREATE TABLE node_state (id INTEGER PRIMARY KEY, state VARCHAR(20));");
        if (MetaRead("SELECT COUNT(*) FROM cluster;").tuples.at(0).GetValue(0).GetAsInt() == 0)
            MetaWrite("INSERT INTO cluster VALUES (" + std::to_string(options_.shard_count) + ", " +
                      std::to_string(options_.replication_factor) + ", 0, FALSE);");
        // Fence off the previous leader: storage nodes refuse older epochs
        MetaWrite("UPDATE cluster SET epoch = epoch + 1;");
        connections_.clear();
        unreachable_until_.clear();
        last_seen_.clear(); // every node gets a full grace period under a new leader
        shard_leaders_.clear();
        ensured_.clear();
        last_full_ensure_ = {};
        // Whatever an earlier term left open on the shards is settled anew
        active_txns_.clear();
        explicit_txns_.clear();
        waits_for_.clear();
        unsettled_.clear();
        for (uint32_t s = 0; s < options_.shard_count; ++s)
            unsettled_.insert(s);
        last_recover_ = {};
        Load();
        for (const auto &[id, node] : view_.nodes)
            last_seen_[id] = Clock::now();
        was_leader_ = true;
    }

    void Coordinator::Load()
    {
        View view;
        const auto cluster = MetaRead("SELECT shards, rf, epoch, formed FROM cluster;");
        const Tuple &row = cluster.tuples.at(0);
        view.shards = static_cast<uint32_t>(row.GetValue(0).GetAsInt());
        view.rf = static_cast<size_t>(row.GetValue(1).GetAsInt());
        view.epoch = static_cast<uint64_t>(row.GetValue(2).GetAsInt());
        view.formed = row.GetValue(3).GetAsBool();
        for (const auto &t : MetaRead("SELECT id, name, host, port, raft_base FROM nodes;").tuples)
        {
            NodeInfo node{t.GetValue(0).GetAsInt(), Text(t.GetValue(1)), Text(t.GetValue(2)),
                          static_cast<uint16_t>(t.GetValue(3).GetAsInt()), t.GetValue(4).GetAsInt()};
            view.nodes[node.id] = node;
        }
        for (const auto &t : MetaRead("SELECT shard, node FROM replicas ORDER BY shard, position;").tuples)
            view.replicas[static_cast<uint32_t>(t.GetValue(0).GetAsInt())].push_back(t.GetValue(1).GetAsInt());
        for (const auto &t : MetaRead("SELECT name, definition, kind, shard_column FROM tables;").tuples)
        {
            TableInfo table{Text(t.GetValue(0)), Text(t.GetValue(1)), Text(t.GetValue(2)), Text(t.GetValue(3))};
            view.tables[table.name] = table;
        }
        for (const auto &t : MetaRead("SELECT seq, statement FROM ddl ORDER BY seq;").tuples)
            view.ddl.emplace_back(t.GetValue(0).GetAsInt(), Text(t.GetValue(1)));
        for (const auto &t : MetaRead("SELECT shard, applied FROM shard_ddl;").tuples)
            view.shard_ddl[static_cast<uint32_t>(t.GetValue(0).GetAsInt())] = t.GetValue(1).GetAsInt();
        for (const auto &t : MetaRead("SELECT id FROM node_state WHERE state = 'removed';").tuples)
            view.removed.insert(t.GetValue(0).GetAsInt());
        view_ = std::move(view);
    }

    void Coordinator::Reconcile()
    {
        Form();
        if (!view_.formed)
            return;
        const bool force = Clock::now() - last_full_ensure_ > std::chrono::seconds(5);
        EnsureReplicas(force);
        if (force)
            last_full_ensure_ = Clock::now();
        CatchUpDdl();
        ExpireIdleTransactions();
        if (!unsettled_.empty() && Clock::now() - last_recover_ > std::chrono::seconds(1))
        {
            last_recover_ = Clock::now();
            Recover();
        }
        Rebalance();
    }

    void Coordinator::Form()
    {
        if (view_.formed || view_.nodes.size() < view_.rf)
            return;
        Placement placement(view_.shards, view_.rf);
        for (const auto &[id, node] : view_.nodes)
            placement.AddNode(static_cast<NodeId>(id), node.name);
        // Start over if an earlier leader died part-way through
        MetaWrite("DELETE FROM replicas;");
        MetaWrite("DELETE FROM shard_ddl;");
        std::string replicas, progress;
        for (const auto &[shard, nodes] : placement.Assignment())
        {
            for (size_t i = 0; i < nodes.size(); ++i)
                replicas += std::string(replicas.empty() ? "" : ", ") + "(" + std::to_string(shard) + ", " +
                            std::to_string(i) + ", " + std::to_string(nodes[i]) + ")";
            progress += std::string(progress.empty() ? "" : ", ") + "(" + std::to_string(shard) + ", 0)";
        }
        MetaWrite("INSERT INTO replicas VALUES " + replicas + ";");
        MetaWrite("INSERT INTO shard_ddl VALUES " + progress + ";");
        MetaWrite("UPDATE cluster SET formed = TRUE;");
        Load();
    }

    std::vector<uint32_t> Coordinator::AllShards() const
    {
        std::vector<uint32_t> shards;
        for (uint32_t s = 0; s < view_.shards; ++s)
            shards.push_back(s);
        return shards;
    }

    net::RemoteConnection *Coordinator::NodeConnection(int node)
    {
        auto it = connections_.find(node);
        if (it != connections_.end())
            return it->second.get();
        auto info = view_.nodes.find(node);
        if (info == view_.nodes.end())
            return nullptr;
        // A node that just failed to answer is left alone for a moment: every
        // try costs up to the connect timeout, with every client waiting
        auto backoff = unreachable_until_.find(node);
        if (backoff != unreachable_until_.end() && Clock::now() < backoff->second)
            return nullptr;
        try
        {
            auto connection = std::make_unique<net::RemoteConnection>(info->second.host, info->second.port);
            unreachable_until_.erase(node);
            return (connections_[node] = std::move(connection)).get();
        }
        catch (const std::exception &)
        {
            unreachable_until_[node] = Clock::now() + std::chrono::seconds(3);
            return nullptr;
        }
    }

    void Coordinator::EnsureReplicas(bool force)
    {
        for (const auto &[shard, nodes] : view_.replicas)
        {
            std::vector<ReplicatedShard::Member> members;
            for (int node : nodes)
            {
                const NodeInfo &info = view_.nodes.at(node);
                members.push_back({node, info.host + ":" + std::to_string(info.raft_base + static_cast<int>(shard))});
            }
            for (int node : nodes)
            {
                if (!force && ensured_[{shard, node}])
                    continue;
                net::RemoteConnection *connection = NodeConnection(node);
                if (!connection)
                    continue;
                try
                {
                    const auto result = connection->Call(
                        "ensure_replica", {std::to_string(view_.epoch), std::to_string(shard), EncodeMembers(members)});
                    ensured_[{shard, node}] = result.success;
                }
                catch (const std::exception &)
                {
                    connections_.erase(node);
                    ensured_[{shard, node}] = false;
                }
            }
        }
    }

    ExecutionResult Coordinator::ShardCall(uint32_t shard, const std::string &method, const std::string &sql,
                                           const std::string &op, const std::string &txid, int timeout_ms)
    {
        if (method == "read")
            ++shard_reads_;
        const auto deadline = Clock::now() + std::chrono::milliseconds(
                                                 timeout_ms > 0 ? timeout_ms : options_.election_timeout_ms * 6 + 2000);
        std::string last_error = "no replica of shard " + std::to_string(shard) + " answered";
        // One id for every attempt: a write retried after an uncertain
        // failure applies at most once
        const std::string request_id =
            method == "write" || method == "txn" ? ReplicatedShard::NewRequestId() : "";
        std::vector<std::string> call = {std::to_string(shard)};
        if (method == "txn")
            call.insert(call.end(), {op, txid, sql, request_id});
        else if (method == "txn_status")
            call.push_back("-");
        else
        {
            call.push_back(sql);
            if (method == "write")
                call.push_back(request_id);
            else if (method == "read" && txn_ && txn_->Open())
                call.push_back(txn_->id); // a read sees its transaction's changes
        }
        int hint = -1;
        while (Clock::now() < deadline)
        {
            std::vector<int> order;
            for (int preferred : {hint, shard_leaders_.count(shard) ? shard_leaders_[shard] : -1})
                if (preferred >= 0)
                    order.push_back(preferred);
            for (int node : view_.replicas[shard])
                if (std::find(order.begin(), order.end(), node) == order.end())
                    order.push_back(node);
            hint = -1;
            for (int node : order)
            {
                net::RemoteConnection *connection = NodeConnection(node);
                if (!connection)
                    continue;
                ExecutionResult result;
                try
                {
                    std::vector<std::string> arguments = {std::to_string(view_.epoch)};
                    arguments.insert(arguments.end(), call.begin(), call.end());
                    result = connection->Call(method, arguments);
                }
                catch (const std::exception &e)
                {
                    connections_.erase(node);
                    last_error = e.what();
                    continue;
                }
                if (result.success || !IsUnavailable(result.message))
                {
                    shard_leaders_[shard] = node;
                    return result; // the SQL's own outcome
                }
                last_error = result.message;
                if (result.message.find("stale epoch") != std::string::npos)
                    throw std::runtime_error("this coordinator was replaced by a newer leader");
                if (result.message.find("no replica of shard") != std::string::npos)
                    ensured_[{shard, node}] = false;
                const int leader = LeaderHint(result.message);
                if (leader > 0 && leader != node)
                {
                    hint = leader;
                    break;
                }
            }
            if (hint < 0)
            {
                EnsureReplicas(false);
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
        // Unavailable: marked so callers can tell it from a SQL failure
        return Failure("shard " + std::to_string(shard) + " is unavailable (not committed): " + last_error);
    }

    void Coordinator::CatchUpDdl()
    {
        if (view_.ddl.empty())
            return;
        for (uint32_t shard : AllShards())
        {
            for (const auto &[seq, statement] : view_.ddl)
            {
                if (seq <= view_.shard_ddl[shard])
                    continue;
                const ExecutionResult result = ShardCall(shard, "write", statement);
                // A shard that cannot be reached is retried next round; a
                // statement that ran (even if it failed there) is done
                if (!result.success && IsUnavailable(result.message))
                    break;
                MetaWrite("UPDATE shard_ddl SET applied = " + std::to_string(seq) +
                          " WHERE shard = " + std::to_string(shard) + ";");
                view_.shard_ddl[shard] = seq;
            }
        }
    }

    std::unique_ptr<Database> Coordinator::SchemaDatabase()
    {
        auto db = Database::OpenInMemory();
        Session session(db.get());
        for (const auto &[seq, statement] : view_.ddl)
            session.Execute(statement);
        if (txn_)
            for (const auto &statement : txn_->ddl)
                session.Execute(statement);
        return db;
    }

    Coordinator::TableInfo Coordinator::DescribeTable(const std::string &sql, const CreateTableStatement &create)
    {
        std::string kind = "row", column;
        if (create.distribution == TableDistribution::REPLICATED)
            kind = "replicated";
        else if (create.distribution == TableDistribution::HASH)
            kind = "hash", column = create.shard_column;
        else
        {
            for (const auto &col : create.columns)
                if (col.primary_key)
                    kind = "hash", column = col.name;
        }
        return TableInfo{create.table, sql, kind, column};
    }

    const std::map<std::string, Coordinator::TableInfo> &Coordinator::Tables()
    {
        if (!txn_ || txn_->ddl.empty())
            return view_.tables;
        // Built once per change, so references into it stay valid
        const std::string key = txn_->id + "#" + std::to_string(txn_->ddl.size()) + "#" + std::to_string(view_.ddl.size());
        if (key == overlay_key_)
            return overlay_tables_;
        overlay_key_ = key;
        overlay_tables_ = view_.tables;
        for (const auto &statement : txn_->ddl)
        {
            std::string error;
            auto stmt = ParseOne(statement, &error);
            if (stmt && stmt->GetType() == StatementType::CREATE_TABLE)
            {
                const auto *create = static_cast<const CreateTableStatement *>(stmt.get());
                overlay_tables_[create->table] = DescribeTable(statement, *create);
            }
            else if (stmt && stmt->GetType() == StatementType::DROP_TABLE)
                overlay_tables_.erase(static_cast<const DropTableStatement *>(stmt.get())->table);
        }
        return overlay_tables_;
    }

    bool Coordinator::Formed()
    {
        std::lock_guard<std::mutex> lock(op_mutex_);
        return view_.formed;
    }

    size_t Coordinator::NodeCount()
    {
        std::lock_guard<std::mutex> lock(op_mutex_);
        return view_.nodes.size();
    }

    uint64_t Coordinator::Epoch()
    {
        std::lock_guard<std::mutex> lock(op_mutex_);
        return view_.epoch;
    }

    ExecutionResult Coordinator::Call(const std::string &method, const std::vector<std::string> &arguments)
    {
        std::lock_guard<std::mutex> lock(op_mutex_);
        if (!IsLeader())
            return Failure("not the leader (leader: " + std::to_string(metadata_ ? metadata_->LeaderId() : -1) + ")");
        try
        {
            if (!was_leader_)
                BecomeLeader();
            if (method == "join")
                return Join(arguments);
            return Failure("unknown call " + method);
        }
        catch (const std::exception &e)
        {
            was_leader_ = false;
            return Failure(e.what());
        }
    }

    ExecutionResult Coordinator::Join(const std::vector<std::string> &arguments)
    {
        if (arguments.size() != 4)
            return Failure("join needs name, host, port, raft_base");
        const std::string &name = arguments[0], &host = arguments[1];
        const int port = std::stoi(arguments[2]), raft_base = std::stoi(arguments[3]);
        for (const auto &[id, node] : view_.nodes)
        {
            if (node.name != name)
                continue;
            if (view_.removed.count(id))
                return Failure("node " + name + " was removed from the cluster; join under a new name");
            unreachable_until_.erase(id); // it is back
            last_seen_[id] = Clock::now();
            if (node.host != host || node.port != port || node.raft_base != raft_base)
            {
                MetaWrite("UPDATE nodes SET host = " + Quote(host) + ", port = " + std::to_string(port) +
                          ", raft_base = " + std::to_string(raft_base) + " WHERE id = " + std::to_string(id) + ";");
                connections_.erase(id);
                Load();
            }
            return Success(std::to_string(id));
        }
        int id = 1;
        for (const auto &[existing, node] : view_.nodes)
            id = std::max(id, existing + 1);
        MetaWrite("INSERT INTO nodes VALUES (" + std::to_string(id) + ", " + Quote(name) + ", " + Quote(host) + ", " +
                  std::to_string(port) + ", " + std::to_string(raft_base) + ");");
        Load();
        last_seen_[id] = Clock::now();
        return Success(std::to_string(id));
    }

    namespace
    {
        // The AND-ed parts of a condition
        void Conjuncts(const Expression *expr, std::vector<const Expression *> *out)
        {
            if (!expr)
                return;
            if (expr->GetType() == ExpressionType::BINARY_OP &&
                static_cast<const BinaryExpression *>(expr)->op == TokenType::AND)
            {
                Conjuncts(static_cast<const BinaryExpression *>(expr)->left.get(), out);
                Conjuncts(static_cast<const BinaryExpression *>(expr)->right.get(), out);
                return;
            }
            out->push_back(expr);
        }

        void ColumnNames(const Expression *expr, std::vector<std::string> *out)
        {
            if (!expr)
                return;
            if (expr->GetType() == ExpressionType::COLUMN_REF)
                out->push_back(static_cast<const ColumnExpression *>(expr)->name);
            for (const Expression *child : ExpressionChildren(expr))
                ColumnNames(child, out);
        }

        // The shards a condition on a hash table can match, from conjuncts
        // "key = literal" and "key IN (literals)"; nullopt if it can match
        // rows on any shard
        std::optional<std::set<uint32_t>> KeyShards(const std::vector<const Expression *> &conjuncts,
                                                    const std::string &shard_column, const Placement &keys)
        {
            auto bare = [](const std::string &name)
            {
                const size_t dot = name.find('.');
                return dot == std::string::npos ? name : name.substr(dot + 1);
            };
            auto is_key = [&](const Expression *e)
            {
                return e->GetType() == ExpressionType::COLUMN_REF &&
                       bare(static_cast<const ColumnExpression *>(e)->name) == shard_column;
            };
            std::optional<std::set<uint32_t>> result;
            for (const Expression *c : conjuncts)
            {
                std::vector<Value> keys_seen;
                bool prunes = false;
                if (c->GetType() == ExpressionType::BINARY_OP)
                {
                    const auto *b = static_cast<const BinaryExpression *>(c);
                    if (b->op == TokenType::EQ)
                    {
                        if (is_key(b->left.get()) && b->right->GetType() == ExpressionType::LITERAL)
                            keys_seen.push_back(static_cast<const LiteralExpression *>(b->right.get())->value), prunes = true;
                        else if (is_key(b->right.get()) && b->left->GetType() == ExpressionType::LITERAL)
                            keys_seen.push_back(static_cast<const LiteralExpression *>(b->left.get())->value), prunes = true;
                    }
                }
                else if (c->GetType() == ExpressionType::IN_LIST)
                {
                    const auto *in = static_cast<const InListExpression *>(c);
                    prunes = !in->negated && is_key(in->operand.get()) &&
                             std::all_of(in->list.begin(), in->list.end(),
                                         [](const auto &e) { return e->GetType() == ExpressionType::LITERAL; });
                    if (prunes)
                        for (const auto &e : in->list)
                            keys_seen.push_back(static_cast<const LiteralExpression *>(e.get())->value);
                }
                if (!prunes)
                    continue;
                std::set<uint32_t> allowed;
                for (const Value &key : keys_seen)
                    if (!key.IsNull()) // key = NULL matches no row
                        allowed.insert(keys.ShardFor(key));
                if (!result)
                    result = allowed;
                else
                {
                    std::set<uint32_t> both;
                    for (uint32_t s : allowed)
                        if (result->count(s))
                            both.insert(s);
                    result = both;
                }
            }
            return result;
        }

        std::string List(const std::vector<uint32_t> &shards)
        {
            std::string text;
            for (uint32_t s : shards)
                text += (text.empty() ? "" : ",") + std::to_string(s);
            return "[" + text + "]";
        }
    } // namespace

    ExecutionResult Coordinator::Execute(const std::string &sql, Transaction *txn)
    {
        // A statement that finds a shard locked by another transaction is
        // undone and tried again (letting that transaction finish) until
        // the lock timeout
        const auto deadline = Clock::now() + std::chrono::milliseconds(options_.lock_timeout_ms);
        auto pause = std::chrono::milliseconds(5);
        static thread_local std::mt19937 random(std::random_device{}());
        for (;;)
        {
            ExecutionResult result;
            {
                std::lock_guard<std::mutex> lock(op_mutex_);
                result = ExecuteLocked(sql, txn);
            }
            if (!IsBusy(result))
                return result;
            if (Clock::now() >= deadline)
            {
                result.message = "lock wait timed out: " + result.message;
                std::lock_guard<std::mutex> lock(op_mutex_);
                if (txn && txn->Open())
                {
                    waits_for_.erase(txn->id);
                    result.message += KEPT_OPEN;
                }
                return result;
            }
            std::this_thread::sleep_for(pause + std::chrono::milliseconds(random() % 10));
            pause = std::min(pause * 2, std::chrono::milliseconds(100));
        }
    }

    void Coordinator::Abandon(Transaction *txn)
    {
        if (!txn)
            return;
        std::lock_guard<std::mutex> lock(op_mutex_);
        explicit_txns_.erase(txn);
        if (!txn->Open())
            return;
        try
        {
            if (IsLeader() && was_leader_ && txn->epoch == view_.epoch)
                Abort(txn);
        }
        catch (const std::exception &)
        {
            // the next leader settles it
        }
        *txn = Transaction();
    }

    bool Coordinator::InTransaction(const Transaction *txn)
    {
        std::lock_guard<std::mutex> lock(op_mutex_);
        return txn->Open() || !txn->expired.empty();
    }

    bool Coordinator::Drop(Transaction *txn)
    {
        std::lock_guard<std::mutex> lock(op_mutex_);
        explicit_txns_.erase(txn);
        const bool open = txn->Open() || !txn->expired.empty();
        *txn = Transaction();
        return open;
    }

    ExecutionResult Coordinator::ExecuteLocked(const std::string &sql, Transaction *txn)
    {
        if (!IsLeader())
            return Failure("not the leader (leader: " + std::to_string(metadata_ ? metadata_->LeaderId() : -1) + ")");
        struct Reset
        {
            Transaction *&slot;
            ~Reset() { slot = nullptr; }
        } reset{txn_};
        try
        {
            if (!was_leader_)
                BecomeLeader();
            std::string error;
            auto stmt = ParseOne(sql, &error);
            if (txn && !txn->expired.empty())
            {
                // Said once, in place of the statement (ROLLBACK just agrees)
                const std::string why = txn->expired;
                txn->expired.clear();
                if (stmt && stmt->GetType() == StatementType::TRANSACTION_STMT &&
                    static_cast<const struct TransactionStatement *>(stmt.get())->kind ==
                        TransactionStatement::Kind::ROLLBACK)
                    return Success("Transaction rolled back.");
                return Failure(why + "; this statement was not run");
            }
            if (txn && txn->Open())
                txn->last_used = Clock::now();
            if (txn && txn->Open() && txn->epoch != view_.epoch)
            {
                // A new term settled (rolled back) everything left open
                *txn = Transaction();
                if (stmt && stmt->GetType() == StatementType::TRANSACTION_STMT &&
                    static_cast<const struct TransactionStatement *>(stmt.get())->kind ==
                        TransactionStatement::Kind::ROLLBACK)
                    return Success("Transaction rolled back.");
                return Failure("the transaction was rolled back: the coordinator lost the leadership");
            }
            if (!stmt)
                return Failure(error);
            if (stmt->GetType() == StatementType::TRANSACTION_STMT)
                return RunTransactionStatement(*static_cast<const struct TransactionStatement *>(stmt.get()), txn);
            if (txn && txn->Open())
                txn_ = txn;
            const bool in_transaction = txn_ != nullptr;
            ExecutionResult result;
            switch (stmt->GetType())
            {
            case StatementType::CREATE_TABLE:
            case StatementType::DROP_TABLE:
            case StatementType::CREATE_INDEX:
                result = Ddl(sql);
                break;
            default:
                if (!view_.formed)
                    return Failure("the cluster is still forming: " + std::to_string(view_.nodes.size()) + " of " +
                                   std::to_string(view_.rf) + " storage nodes have joined");
                switch (stmt->GetType())
                {
                case StatementType::INSERT:
                    result = Insert(sql);
                    break;
                case StatementType::UPDATE_STMT:
                {
                    const auto *update = static_cast<const UpdateStatement *>(stmt.get());
                    const auto &tables = Tables();
                    auto table = tables.find(update->table);
                    bool subqueries = ContainsSubquery(update->where.get());
                    for (const auto &[column, expr] : update->assignments)
                    {
                        subqueries = subqueries || ContainsSubquery(expr.get());
                        if (table != tables.end() && table->second.kind == "hash" &&
                            column == table->second.shard_column)
                            return Failure("cannot change the shard key " + column + " of " + update->table);
                    }
                    std::string error;
                    const std::string bound = subqueries ? BoundUpdate(*update, &error) : sql;
                    result = error.empty() ? Broadcast(bound, update->table) : Failure(error);
                    break;
                }
                case StatementType::DELETE_STMT:
                {
                    const auto *del = static_cast<const DeleteStatement *>(stmt.get());
                    std::string error;
                    const std::string bound = ContainsSubquery(del->where.get()) ? BoundDelete(*del, &error) : sql;
                    result = error.empty() ? Broadcast(bound, del->table) : Failure(error);
                    break;
                }
                case StatementType::ANALYZE_STMT:
                {
                    std::map<uint32_t, std::string> statements;
                    for (uint32_t shard : AllShards())
                        statements[shard] = sql;
                    std::map<uint32_t, ExecutionResult> results;
                    result = Atomically([&] { return WriteShards(statements, false, &results); });
                    if (result.success && results.count(0))
                        result = results[0];
                    break;
                }
                default: // SELECT, EXPLAIN
                    result = Select(sql);
                    break;
                }
            }
            if (in_transaction && txn->Open())
            {
                if (!IsBusy(result))
                    waits_for_.erase(txn->id);
                else if (!txn->shards.empty())
                {
                    // It holds shards and waits for one: a deadlock if the
                    // holder (transitively) waits for it
                    const size_t open = result.message.rfind('('), close = result.message.rfind(')');
                    std::string holder = open != std::string::npos && close > open
                                             ? result.message.substr(open + 1, close - open - 1)
                                             : "";
                    waits_for_[txn->id] = holder;
                    for (size_t hops = 0; !holder.empty() && hops <= waits_for_.size(); ++hops)
                    {
                        if (holder == txn->id)
                        {
                            Abort(txn);
                            return Failure("Deadlock detected: the transaction was rolled back; retry it");
                        }
                        auto next = waits_for_.find(holder);
                        holder = next == waits_for_.end() ? "" : next->second;
                    }
                }
            }
            // As a single node reports a failed statement inside BEGIN
            if (in_transaction && !result.success && !IsBusy(result) && txn->Open() &&
                result.message.find(KEPT_OPEN) == std::string::npos)
                result.message += KEPT_OPEN;
            return result;
        }
        catch (const std::exception &e)
        {
            was_leader_ = false;
            return Failure(e.what());
        }
    }

    ExecutionResult Coordinator::RunTransactionStatement(const struct TransactionStatement &stmt, Transaction *txn)
    {
        switch (stmt.kind)
        {
        case TransactionStatement::Kind::BEGIN:
            if (!txn)
                return Failure("transactions need a client connection");
            if (txn->Open())
                return Failure("A transaction is already in progress.");
            if (!view_.formed)
                return Failure("the cluster is still forming: " + std::to_string(view_.nodes.size()) + " of " +
                               std::to_string(view_.rf) + " storage nodes have joined");
            *txn = Transaction();
            txn->id = "c" + std::to_string(options_.id) + "-" + ReplicatedShard::NewRequestId();
            txn->epoch = view_.epoch;
            txn->last_used = Clock::now();
            active_txns_.insert(txn->id);
            explicit_txns_.insert(txn);
            return Success("Transaction started.");
        case TransactionStatement::Kind::COMMIT:
            if (!txn || !txn->Open())
                return Failure("No transaction is in progress.");
            return Commit(txn);
        case TransactionStatement::Kind::ROLLBACK:
            if (!txn || !txn->Open())
                return Failure("No transaction is in progress.");
            Abort(txn);
            return Success("Transaction rolled back.");
        }
        return Failure("unknown transaction statement");
    }

    ExecutionResult Coordinator::Atomically(const std::function<ExecutionResult()> &body)
    {
        if (txn_)
            return body();
        Transaction implicit;
        implicit.id = "c" + std::to_string(options_.id) + "-" + ReplicatedShard::NewRequestId();
        implicit.epoch = view_.epoch;
        implicit.implicit = true;
        active_txns_.insert(implicit.id);
        txn_ = &implicit;
        ExecutionResult result;
        try
        {
            result = body();
        }
        catch (...)
        {
            txn_ = nullptr;
            unsettled_.insert(implicit.shards.begin(), implicit.shards.end());
            active_txns_.erase(implicit.id);
            throw;
        }
        txn_ = nullptr;
        if (!result.success)
        {
            Abort(&implicit);
            return result;
        }
        ExecutionResult committed = Commit(&implicit);
        return committed.success ? result : committed;
    }

    ExecutionResult Coordinator::WriteShards(const std::map<uint32_t, std::string> &statements, bool release_untouched,
                                             std::map<uint32_t, ExecutionResult> *results)
    {
        if (!txn_)
        {
            if (statements.size() != 1)
                throw std::logic_error("a write to several shards needs a transaction");
            const auto &[shard, sql] = *statements.begin();
            ExecutionResult result = ShardCall(shard, "write", sql);
            (*results)[shard] = result;
            return result;
        }
        Transaction &t = *txn_;
        const std::set<uint32_t> before = t.shards;
        std::vector<uint32_t> ran; // where the statement succeeded
        ExecutionResult failure;
        bool failed = false, uncertain = false;
        for (const auto &[shard, sql] : statements)
        {
            ExecutionResult result = ShardCall(shard, "txn", sql, "exec", t.id);
            if (!IsBusy(result))
                t.shards.insert(shard); // the shard has the transaction open (or may have)
            if (!result.success)
            {
                failure = result;
                failed = true;
                uncertain = IsUnavailable(result.message);
                break;
            }
            ran.push_back(shard);
            (*results)[shard] = result;
        }
        if (failed)
        {
            if (t.implicit)
                return failure; // all of it is rolled back
            // Undo the statement where it ran; let go of shards it opened
            bool clean = !uncertain;
            for (uint32_t shard : ran)
                clean = ShardCall(shard, "txn", "", "undo", t.id).success && clean;
            if (!clean)
            {
                Abort(&t);
                failure.message += " (the transaction was rolled back)";
                return failure;
            }
            for (uint32_t shard : std::set<uint32_t>(t.shards))
                if (!before.count(shard) && ShardCall(shard, "txn", "", "abort", t.id).success)
                    t.shards.erase(shard);
            return failure;
        }
        if (release_untouched)
            for (uint32_t shard : ran)
                if (!before.count(shard) && Count((*results)[shard].message) == 0 &&
                    ShardCall(shard, "txn", "", "abort", t.id).success)
                    t.shards.erase(shard); // changed nothing there
        return Success("");
    }

    ExecutionResult Coordinator::Commit(Transaction *txn)
    {
        explicit_txns_.erase(txn);
        Transaction t = *txn;
        *txn = Transaction();
        active_txns_.erase(t.id);
        waits_for_.erase(t.id);
        if (t.shards.empty() && t.ddl.empty())
            return Success("Transaction committed.");
        if (t.shards.size() == 1 && t.ddl.empty())
        {
            // One participant: its commit is the decision
            const uint32_t shard = *t.shards.begin();
            ExecutionResult result = ShardCall(shard, "txn", "", "commit", t.id);
            if (result.success)
                return Success("Transaction committed.");
            if (IsUnavailable(result.message))
            {
                unsettled_.insert(shard);
                return Failure("the commit's outcome is unknown: " + result.message);
            }
            ShardCall(shard, "txn", "", "abort", t.id);
            return Failure("Commit failed; the transaction was rolled back (" + result.message + ")");
        }
        // Phase one: every participant votes
        for (uint32_t shard : t.shards)
        {
            ExecutionResult vote = ShardCall(shard, "txn", "", "prepare", t.id);
            if (!vote.success)
            {
                Abort(&t);
                return Failure("Commit failed; the transaction was rolled back (shard " + std::to_string(shard) +
                               ": " + vote.message + ")");
            }
        }
        // The decision, with the transaction's schema changes: the commit point
        std::string decision = "INSERT INTO txns VALUES (" + Quote(t.id) + ", 'committed');\n";
        int64_t seq = view_.ddl.empty() ? 0 : view_.ddl.back().first;
        for (const auto &statement : t.ddl)
        {
            std::string error;
            auto stmt = ParseOne(statement, &error);
            if (stmt->GetType() == StatementType::CREATE_TABLE)
            {
                const TableInfo info =
                    DescribeTable(statement, *static_cast<const CreateTableStatement *>(stmt.get()));
                decision += "DELETE FROM tables WHERE name = " + Quote(info.name) + ";\n";
                decision += "INSERT INTO tables VALUES (" + Quote(info.name) + ", " + Quote(statement) + ", " +
                            Quote(info.kind) + ", " + Quote(info.shard_column) + ");\n";
            }
            else if (stmt->GetType() == StatementType::DROP_TABLE)
                decision += "DELETE FROM tables WHERE name = " +
                            Quote(static_cast<const DropTableStatement *>(stmt.get())->table) + ";\n";
            ++seq;
            decision += "INSERT INTO ddl VALUES (" + std::to_string(seq) + ", " + Quote(statement) + ");\n";
        }
        if (!t.ddl.empty())
            decision += "UPDATE shard_ddl SET applied = " + std::to_string(seq) + ";\n";
        ExecutionResult decided;
        try
        {
            decided = MetaWrite(decision, true);
        }
        catch (const std::exception &e)
        {
            // Maybe written: the next leader looks it up
            unsettled_.insert(t.shards.begin(), t.shards.end());
            throw std::runtime_error(std::string("the commit's outcome is unknown: ") + e.what());
        }
        if (!decided.success)
        {
            // e.g. a recovering leader already decided to roll it back
            Abort(&t);
            return Failure("Commit failed; the transaction was rolled back (" + decided.message + ")");
        }
        ++two_phase_commits_;
        // Phase two
        bool everywhere = true;
        for (uint32_t shard : t.shards)
        {
            ExecutionResult done = ShardCall(shard, "txn", "", "commit", t.id);
            if (!done.success)
            {
                everywhere = false; // recovery finishes it
                unsettled_.insert(shard);
            }
        }
        if (everywhere)
            MetaWrite("DELETE FROM txns WHERE id = " + Quote(t.id) + ";");
        if (!t.ddl.empty())
            Load();
        return Success("Transaction committed.");
    }

    void Coordinator::Abort(Transaction *txn)
    {
        explicit_txns_.erase(txn);
        Transaction t = *txn;
        *txn = Transaction();
        active_txns_.erase(t.id);
        waits_for_.erase(t.id);
        for (uint32_t shard : t.shards)
            if (!ShardCall(shard, "txn", "", "abort", t.id).success)
                unsettled_.insert(shard); // recovery rolls it back
    }

    std::vector<int> Coordinator::ActiveNodes() const
    {
        std::vector<int> active;
        const auto limit = std::chrono::milliseconds(options_.node_down_after_ms);
        for (const auto &[id, node] : view_.nodes)
        {
            auto seen = last_seen_.find(id);
            if (!view_.removed.count(id) && seen != last_seen_.end() && Clock::now() - seen->second < limit)
                active.push_back(id);
        }
        return active;
    }

    bool Coordinator::DropReplica(uint32_t shard, int node)
    {
        net::RemoteConnection *connection = NodeConnection(node);
        bool dropped = false;
        if (connection)
        {
            try
            {
                dropped =
                    connection->Call("drop_replica", {std::to_string(view_.epoch), std::to_string(shard), "-"}).success;
            }
            catch (const std::exception &)
            {
                connections_.erase(node);
            }
        }
        if (dropped)
            pending_drops_.erase({shard, node});
        else
            pending_drops_.insert({shard, node});
        return dropped;
    }

    void Coordinator::Rebalance()
    {
        if (!view_.formed)
            return;
        // Copies left on nodes that were unreachable when they were removed
        for (const auto &[shard, node] : std::set<std::pair<uint32_t, int>>(pending_drops_))
            if (view_.nodes.count(node))
                DropReplica(shard, node);
            else
                pending_drops_.erase({shard, node});
        const std::vector<int> active = ActiveNodes();
        if (active.size() < view_.rf)
            return; // the replicas have nowhere else to go
        Placement placement(view_.shards, view_.rf);
        for (int id : active)
            placement.AddNode(static_cast<NodeId>(id), view_.nodes.at(id).name);
        const auto wanted = placement.Assignment();
        // The first shard (from a rotating start, so one stuck shard does not
        // hold up the rest) whose replicas are not where they should be
        for (uint32_t i = 0; i < view_.shards; ++i)
        {
            const uint32_t shard = (rebalance_cursor_ + i) % view_.shards;
            const auto &want_list = wanted.at(shard);
            const std::set<int> want(want_list.begin(), want_list.end());
            const std::vector<int> have_list = view_.replicas[shard];
            const std::set<int> have(have_list.begin(), have_list.end());
            if (want == have)
                continue;
            rebalance_cursor_ = shard + 1;
            auto endpoint = [&](int node)
            {
                const NodeInfo &info = view_.nodes.at(node);
                return info.host + ":" + std::to_string(info.raft_base + static_cast<int>(shard));
            };
            // Add before removing: the group never has fewer replicas than wanted
            for (int node : want)
            {
                if (have.count(node))
                    continue;
                // A stale copy from an earlier membership goes first
                if (pending_drops_.count({shard, node}) && !DropReplica(shard, node))
                    return;
                // A new replica, joining the group as it is now
                std::vector<ReplicatedShard::Member> members;
                for (int member : have_list)
                    members.push_back({member, endpoint(member)});
                net::RemoteConnection *connection = NodeConnection(node);
                if (!connection)
                    return;
                try
                {
                    if (!connection->Call("ensure_replica", {std::to_string(view_.epoch), std::to_string(shard),
                                                             EncodeMembers(members)})
                             .success)
                        return;
                }
                catch (const std::exception &)
                {
                    connections_.erase(node);
                    return;
                }
                const ExecutionResult added =
                    ShardCall(shard, "add_member", std::to_string(node) + "@" + endpoint(node), "", "", 3000);
                if (!added.success)
                    return; // under way, or the group is unavailable: next round
                const auto next = MetaRead("SELECT MAX(position) FROM replicas WHERE shard = " +
                                           std::to_string(shard) + ";");
                const int position = next.tuples.empty() || next.tuples[0].GetValue(0).IsNull()
                                         ? 0
                                         : next.tuples[0].GetValue(0).GetAsInt() + 1;
                MetaWrite("INSERT INTO replicas VALUES (" + std::to_string(shard) + ", " + std::to_string(position) +
                          ", " + std::to_string(node) + ");");
                ensured_[{shard, node}] = true;
                Load();
                return;
            }
            for (int node : have_list)
            {
                if (want.count(node))
                    continue;
                const ExecutionResult removed = ShardCall(shard, "remove_member", std::to_string(node), "", "", 3000);
                if (!removed.success)
                    return;
                MetaWrite("DELETE FROM replicas WHERE shard = " + std::to_string(shard) +
                          " AND node = " + std::to_string(node) + ";");
                ensured_.erase({shard, node});
                if (shard_leaders_.count(shard) && shard_leaders_[shard] == node)
                    shard_leaders_.erase(shard);
                Load();
                // Its copy is no longer a member: delete it (retried while
                // the node does not answer)
                DropReplica(shard, node);
                return;
            }
        }
    }

    std::string Coordinator::ClusterStatus()
    {
        const std::vector<int> active = ActiveNodes();
        std::map<int, int> held;
        for (const auto &[shard, nodes] : view_.replicas)
            for (int node : nodes)
                ++held[node];
        std::string out = "Nodes:\n";
        for (const auto &[id, node] : view_.nodes)
        {
            std::string state = "active";
            if (view_.removed.count(id))
                state = "removed";
            else if (std::find(active.begin(), active.end(), id) == active.end())
                state = "down";
            else if (Clock::now() - last_seen_[id] > std::chrono::seconds(3))
                state = "silent for " +
                        std::to_string(std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - last_seen_[id]).count()) +
                        " s";
            out += "  " + node.name + " (id " + std::to_string(id) + ") " + node.host + ":" + std::to_string(node.port) +
                   "  " + state + ", " + std::to_string(held[id]) + " replica(s)\n";
        }
        if (!view_.formed)
            return out + "Forming: " + std::to_string(view_.nodes.size()) + " of " + std::to_string(view_.rf) +
                   " storage nodes have joined\n";
        out += "Shards:\n";
        size_t moves = 0;
        Placement placement(view_.shards, view_.rf);
        for (int id : active)
            placement.AddNode(static_cast<NodeId>(id), view_.nodes.at(id).name);
        const auto wanted = placement.Assignment();
        for (const auto &[shard, nodes] : view_.replicas)
        {
            out += "  " + std::to_string(shard) + ":";
            for (int node : nodes)
                out += " " + (view_.nodes.count(node) ? view_.nodes.at(node).name : std::to_string(node));
            out += "\n";
            if (active.size() >= view_.rf)
            {
                const std::set<int> want(wanted.at(shard).begin(), wanted.at(shard).end());
                for (int node : want) // replicas to add...
                    moves += std::count(nodes.begin(), nodes.end(), node) == 0;
                for (int node : nodes) // ...and to remove
                    moves += want.count(node) == 0;
            }
        }
        if (active.size() < view_.rf)
            out += "Only " + std::to_string(active.size()) + " active node(s) for " + std::to_string(view_.rf) +
                   " replicas: replicas stay where they are\n";
        else
            out += moves == 0 ? "Balanced\n"
                              : "Rebalancing: " + std::to_string(moves) + " replica change(s) to go\n";
        return out;
    }

    std::string Coordinator::RemoveNode(const std::string &name)
    {
        for (const auto &[id, node] : view_.nodes)
        {
            if (node.name != name)
                continue;
            if (view_.removed.count(id))
                return "Error: " + name + " is already removed\n";
            std::vector<int> active = ActiveNodes();
            active.erase(std::remove(active.begin(), active.end(), id), active.end());
            if (active.size() < view_.rf)
                return "Error: without " + name + " only " + std::to_string(active.size()) +
                       " active node(s) would remain for " + std::to_string(view_.rf) + " replicas of each shard\n";
            MetaWrite("INSERT INTO node_state VALUES (" + std::to_string(id) + ", 'removed');");
            Load();
            return "Removing " + name + ": its replicas move to the other nodes (see .cluster)\n";
        }
        return "Error: no storage node named " + name + "\n";
    }

    std::map<uint32_t, std::vector<int>> Coordinator::Replicas()
    {
        std::lock_guard<std::mutex> lock(op_mutex_);
        return view_.replicas;
    }

    int Coordinator::NodeIdOf(const std::string &name)
    {
        std::lock_guard<std::mutex> lock(op_mutex_);
        for (const auto &[id, node] : view_.nodes)
            if (node.name == name)
                return id;
        return -1;
    }

    void Coordinator::ExpireIdleTransactions()
    {
        if (options_.idle_transaction_timeout_ms <= 0)
            return;
        const auto limit = std::chrono::milliseconds(options_.idle_transaction_timeout_ms);
        for (Transaction *txn : std::set<Transaction *>(explicit_txns_))
        {
            if (!txn->Open() || Clock::now() - txn->last_used < limit)
                continue;
            Abort(txn);
            txn->expired = "the transaction was rolled back after being idle for " +
                           std::to_string(options_.idle_transaction_timeout_ms / 1000) +
                           " s (it held shards other clients need)";
        }
    }

    void Coordinator::Recover()
    {
        for (uint32_t shard : std::set<uint32_t>(unsettled_))
        {
            const ExecutionResult status = ShardCall(shard, "txn_status", "", "", "", 1000);
            if (!status.success)
                continue; // still unreachable: next round
            const std::string txid = status.message;
            if (txid.empty() || active_txns_.count(txid))
            {
                unsettled_.erase(shard);
                continue;
            }
            // Presumed abort: without a commit decision it is rolled back. The
            // abort is recorded first, so a late commit from an earlier
            // leader cannot win as well.
            const std::string lookup = "SELECT state FROM txns WHERE id = " + Quote(txid) + ";";
            ExecutionResult decision = MetaRead(lookup);
            if (decision.tuples.empty())
            {
                MetaWrite("INSERT INTO txns VALUES (" + Quote(txid) + ", 'aborted');");
                decision = MetaRead(lookup);
            }
            const bool committed =
                !decision.tuples.empty() && decision.tuples[0].GetValue(0).ToString() == "committed";
            const ExecutionResult done = ShardCall(shard, "txn", "", committed ? "commit" : "abort", txid, 1000);
            if (done.success)
                unsettled_.erase(shard);
        }
    }

    ExecutionResult Coordinator::Ddl(const std::string &sql)
    {
        // The same checks and messages as a single node: try it on an empty
        // copy of the schema first
        auto schema = SchemaDatabase();
        Session check(schema.get());
        ExecutionResult result = check.Execute(sql);
        if (!result.success)
            return result;
        if (sql.size() > 1900)
            return Failure("statement too long for the cluster's schema log");
        if (!view_.formed)
        {
            // No shards yet: they get the schema log when they are created
            std::string error;
            auto stmt = ParseOne(sql, &error);
            const int64_t seq = view_.ddl.empty() ? 1 : view_.ddl.back().first + 1;
            if (stmt->GetType() == StatementType::CREATE_TABLE)
            {
                const TableInfo info = DescribeTable(sql, *static_cast<const CreateTableStatement *>(stmt.get()));
                MetaWrite("INSERT INTO tables VALUES (" + Quote(info.name) + ", " + Quote(sql) + ", " +
                          Quote(info.kind) + ", " + Quote(info.shard_column) + ");");
            }
            else if (stmt->GetType() == StatementType::DROP_TABLE)
                MetaWrite("DELETE FROM tables WHERE name = " +
                          Quote(static_cast<const DropTableStatement *>(stmt.get())->table) + ";");
            MetaWrite("INSERT INTO ddl VALUES (" + std::to_string(seq) + ", " + Quote(sql) + ");");
            Load();
            return result;
        }
        // Every shard changes its schema in one distributed transaction (the
        // metadata changes with the commit decision), after any earlier
        // schema change has reached it
        CatchUpDdl();
        const int64_t last = view_.ddl.empty() ? 0 : view_.ddl.back().first;
        for (uint32_t shard : AllShards())
            if (view_.shard_ddl[shard] < last)
                return Failure("shard " + std::to_string(shard) +
                               " is still applying an earlier schema change (locked by another transaction)");
        std::map<uint32_t, std::string> statements;
        for (uint32_t shard : AllShards())
            statements[shard] = sql;
        std::map<uint32_t, ExecutionResult> results;
        ExecutionResult ran = Atomically(
            [&]
            {
                ExecutionResult written = WriteShards(statements, false, &results);
                if (written.success)
                    txn_->ddl.push_back(sql);
                return written;
            });
        return ran.success ? result : ran;
    }

    ExecutionResult Coordinator::Insert(const std::string &sql)
    {
        std::string error;
        auto stmt = ParseOne(sql, &error);
        const auto *insert = static_cast<const InsertStatement *>(stmt.get());
        const auto &tables = Tables();
        auto table = tables.find(insert->table);
        if (table == tables.end())
            return Failure("Table not found: " + insert->table);
        // Run it against an empty copy of the table: the same errors as a
        // single node, and every row's final values (defaults, expressions)
        auto schema = SchemaDatabase();
        Session check(schema.get());
        ExecutionResult checked = check.Execute(sql);
        if (!checked.success)
            return checked;
        const Table *scratch = schema->GetCatalog().GetTable(insert->table);
        const auto &columns = scratch->GetSchema().GetColumns();
        int key = -1;
        for (size_t i = 0; i < columns.size(); ++i)
            if (columns[i].name == table->second.shard_column)
                key = static_cast<int>(i);

        std::map<uint32_t, std::string> per_shard;
        std::string column_list;
        for (const auto &col : columns)
            column_list += (column_list.empty() ? "" : ", ") + col.name;
        for (auto it = scratch->begin(); it != scratch->end(); ++it)
        {
            const Tuple &row = *it;
            std::string values;
            for (size_t i = 0; i < row.GetValueCount(); ++i)
                values += (i ? ", " : "") + RenderValue(row.GetValue(i));
            std::vector<uint32_t> targets;
            if (table->second.kind == "replicated")
                targets = AllShards();
            else
            {
                uint64_t hash;
                if (table->second.kind == "hash" && key >= 0)
                    hash = HashKey(row.GetValue(static_cast<size_t>(key)));
                else
                {
                    std::string whole; // a table with no key: the whole row
                    for (size_t i = 0; i < row.GetValueCount(); ++i)
                        AppendGroupKey(row.GetValue(i), &whole);
                    hash = Hash64(whole);
                }
                Placement keys(view_.shards, view_.rf);
                targets = {keys.ShardForHash(hash)};
            }
            for (uint32_t shard : targets)
                per_shard[shard] += (per_shard[shard].empty() ? "" : ", ") + ("(" + values + ")");
        }
        const bool replicated = table->second.kind == "replicated";
        const std::string table_name = insert->table;
        std::map<uint32_t, std::string> statements;
        for (const auto &[shard, rows] : per_shard)
            statements[shard] = "INSERT INTO " + table_name + " (" + column_list + ") VALUES " + rows + ";";
        if (statements.empty())
            return Success("0 row(s) inserted.");
        std::map<uint32_t, ExecutionResult> results;
        ExecutionResult written = statements.size() == 1 ? WriteShards(statements, false, &results)
                                                         : Atomically([&] { return WriteShards(statements, false, &results); });
        if (!written.success)
            return written;
        long long inserted = 0;
        for (const auto &[shard, result] : results)
            if (!replicated || shard == results.begin()->first)
                inserted += Count(result.message);
        return Success(std::to_string(inserted) + " row(s) inserted.");
    }

    ExecutionResult Coordinator::Broadcast(const std::string &sql, const std::string &table_name)
    {
        const auto &tables = Tables();
        auto table = tables.find(table_name);
        if (table == tables.end())
            return Failure("Table not found: " + table_name);
        const bool replicated = table->second.kind == "replicated";
        auto schema = SchemaDatabase();
        Session check(schema.get());
        ExecutionResult checked = check.Execute(sql); // errors as on a single node
        if (!checked.success)
            return checked;
        // Only the shards its WHERE can match: key = literal, key IN (...)
        std::vector<uint32_t> targets = AllShards();
        if (table->second.kind == "hash")
        {
            std::string error;
            auto stmt = ParseOne(sql, &error);
            const Expression *where = nullptr;
            if (stmt && stmt->GetType() == StatementType::UPDATE_STMT)
                where = static_cast<const UpdateStatement *>(stmt.get())->where.get();
            else if (stmt && stmt->GetType() == StatementType::DELETE_STMT)
                where = static_cast<const DeleteStatement *>(stmt.get())->where.get();
            std::vector<const Expression *> conjuncts;
            Conjuncts(where, &conjuncts);
            if (auto allowed = KeyShards(conjuncts, table->second.shard_column, Placement(view_.shards, view_.rf)))
                targets.assign(allowed->begin(), allowed->end());
        }
        if (targets.empty())
            return checked; // no shard can hold a matching row: "0 row(s) ..."
        std::map<uint32_t, std::string> statements;
        for (uint32_t shard : targets)
            statements[shard] = sql;
        std::map<uint32_t, ExecutionResult> results;
        // A replicated table changes on every shard; otherwise a shard the
        // statement changed nothing on need not stay in the transaction
        ExecutionResult written = statements.size() == 1 ? WriteShards(statements, !replicated, &results)
                                                         : Atomically([&] { return WriteShards(statements, !replicated, &results); });
        if (!written.success)
            return written;
        long long count = 0;
        std::string verb;
        for (const auto &[shard, result] : results)
        {
            if (!replicated || shard == 0)
                count += Count(result.message);
            verb = result.message.substr(result.message.find(' ') + 1);
        }
        return Success(std::to_string(count) + " " + verb);
    }


    Coordinator::SelectPlan Coordinator::PlanSelect(const SelectStatement &select, const std::string &sql,
                                                    Database &schema)
    {
        struct Relation
        {
            std::string table, name;
            bool null_supplying = false; // the right side of a LEFT JOIN
            bool pushdown = false;       // its own conditions may go to its shards
            std::vector<const Expression *> conditions;
            std::vector<uint32_t> shards;
        };
        std::vector<Relation> relations;
        auto add = [&](const std::string &table, const std::string &name, bool null_supplying)
        {
            Relation r;
            r.table = table;
            r.name = name;
            r.null_supplying = null_supplying;
            relations.push_back(std::move(r));
        };
        add(select.table, select.table_alias.empty() ? select.table : select.table_alias, false);
        for (const auto &join : select.joins)
            add(join.right.table, join.right.Name(), join.type == JoinType::LEFT);

        // Every table the statement names, and how often its name appears
        std::map<std::string, int> mentions;
        Lexer lexer(sql);
        for (Token token = lexer.NextToken(); token.type != TokenType::END_OF_FILE; token = lexer.NextToken())
            if (token.type == TokenType::IDENTIFIER && Tables().count(token.value))
                ++mentions[token.value];

        bool subqueries = ContainsSubquery(select.where.get()) || ContainsSubquery(select.having.get());
        for (const auto &item : select.items)
            subqueries = subqueries || ContainsSubquery(item.expr.get());
        for (const auto &join : select.joins)
            subqueries = subqueries || ContainsSubquery(join.on.get());

        // A table's own conditions go to its shards only when it appears
        // once, as a relation: a second appearance (self-join, subquery)
        // needs rows those conditions would drop
        std::map<std::string, int> as_relation;
        for (const auto &r : relations)
            ++as_relation[r.table];
        auto columns_of = [&](const std::string &table)
        {
            std::set<std::string> names;
            if (const Table *t = schema.GetCatalog().GetTable(table))
                for (const auto &col : t->GetSchema().GetColumns())
                    names.insert(col.name);
            return names;
        };
        // The relation a column name belongs to, or -1 if unknown/ambiguous
        auto owner = [&](const std::string &name) -> int
        {
            const size_t dot = name.find('.');
            int found = -1;
            for (size_t i = 0; i < relations.size(); ++i)
            {
                const auto cols = columns_of(relations[i].table);
                const bool match = dot == std::string::npos
                                       ? cols.count(name) > 0
                                       : name.substr(0, dot) == relations[i].name && cols.count(name.substr(dot + 1));
                if (match)
                {
                    if (found >= 0)
                        return -1;
                    found = static_cast<int>(i);
                }
            }
            return found;
        };
        auto bare = [](const std::string &name)
        {
            const size_t dot = name.find('.');
            return dot == std::string::npos ? name : name.substr(dot + 1);
        };

        std::vector<const Expression *> conjuncts;
        Conjuncts(select.where.get(), &conjuncts);
        Placement keys(view_.shards, view_.rf);
        for (size_t i = 0; i < relations.size(); ++i)
        {
            Relation &r = relations[i];
            auto info = Tables().find(r.table);
            if (info == Tables().end())
                continue; // unknown: the local run reports it
            r.pushdown = !r.null_supplying && as_relation[r.table] == 1 && mentions[r.table] == 1;
            r.shards = info->second.kind == "replicated" ? std::vector<uint32_t>{0} : AllShards();
            if (!r.pushdown)
                continue;
            for (const Expression *c : conjuncts)
            {
                if (ContainsSubquery(c) || ContainsAggregate(c))
                    continue;
                std::vector<std::string> names;
                ColumnNames(c, &names);
                if (names.empty() || !std::all_of(names.begin(), names.end(),
                                                  [&](const std::string &n) { return owner(n) == static_cast<int>(i); }))
                    continue;
                r.conditions.push_back(c);
                if (info->second.kind != "hash")
                    continue;
                // Pruning: key = literal, key IN (literals)
                const auto allowed = KeyShards({c}, info->second.shard_column, keys);
                if (!allowed)
                    continue;
                std::vector<uint32_t> kept;
                for (uint32_t s : r.shards)
                    if (allowed->count(s))
                        kept.push_back(s);
                r.shards = kept;
            }
        }

        SelectPlan plan;
        // 1. Every table on one shard: run the whole statement there
        {
            bool single = !mentions.empty();
            int shard = -1;
            for (const auto &[table, count] : mentions)
            {
                const TableInfo &info = Tables().at(table);
                if (info.kind == "replicated")
                    continue;
                auto r = std::find_if(relations.begin(), relations.end(),
                                      [&](const Relation &rel) { return rel.table == table; });
                if (r == relations.end() || !r->pushdown || r->shards.size() != 1 ||
                    (shard >= 0 && static_cast<uint32_t>(shard) != r->shards[0]))
                {
                    single = false;
                    break;
                }
                shard = static_cast<int>(r->shards[0]);
            }
            if (single)
            {
                plan.kind = SelectPlan::Kind::SINGLE_SHARD;
                plan.shard = shard < 0 ? 0 : static_cast<uint32_t>(shard);
                return plan;
            }
        }
        // 2. Aggregates over one table, no grouping: per shard, combined here
        const Relation &first = relations.front();
        const bool one_table = relations.size() == 1 && Tables().count(first.table) &&
                               Tables().at(first.table).kind != "replicated" && first.pushdown && !subqueries;
        bool partial = one_table && select.group_by.empty() && !select.having && select.order_by.empty() &&
                       !select.distinct && !select.limit && select.offset == 0 && !select.select_star &&
                       !select.items.empty();
        std::string parts;
        for (const auto &item : select.items)
        {
            if (!partial)
                break;
            if (item.expr->GetType() != ExpressionType::AGGREGATE)
            {
                partial = false;
                break;
            }
            const auto *agg = static_cast<const AggregateExpression *>(item.expr.get());
            if (agg->distinct || (agg->argument && ContainsAggregate(agg->argument.get())))
            {
                partial = false;
                break;
            }
            const std::string arg = agg->argument ? ExpressionToSQL(agg->argument.get()) : "*";
            const char *name = agg->function == AggregateFunction::COUNT ? "COUNT"
                               : agg->function == AggregateFunction::MIN ? "MIN"
                               : agg->function == AggregateFunction::MAX ? "MAX"
                                                                         : "SUM";
            parts += std::string(parts.empty() ? "" : ", ") + name + "(" + arg + ")";
            if (agg->function == AggregateFunction::AVG)
                parts += ", COUNT(" + arg + ")";
            plan.functions.push_back(agg->function);
        }
        const std::string from = " FROM " + first.table + (first.name != first.table ? " " + first.name : "");
        if (partial)
        {
            plan.kind = SelectPlan::Kind::PARTIAL_AGGREGATE;
            plan.shards = first.shards;
            plan.shard_query = "SELECT " + parts + from +
                               (select.where ? " WHERE " + ExpressionToSQL(select.where.get()) : "") + ";";
            return plan;
        }
        plan.functions.clear();

        // 3. Fetch each table's rows (its own conditions applied on its
        // shards; top-N for ORDER BY ... LIMIT over one table), finish here
        bool top_n = one_table && select.group_by.empty() && !select.having && !select.distinct && select.limit &&
                     !select.order_by.empty();
        for (const auto &item : select.items)
            top_n = top_n && !ContainsAggregate(item.expr.get());
        std::string order;
        for (const auto &o : select.order_by)
        {
            if (!top_n)
                break;
            // Only plain columns of the table (not positions or SELECT aliases)
            if (o.expr->GetType() != ExpressionType::COLUMN_REF || owner(static_cast<const ColumnExpression *>(o.expr.get())->name) != 0)
            {
                top_n = false;
                break;
            }
            const std::string &name = static_cast<const ColumnExpression *>(o.expr.get())->name;
            for (const auto &item : select.items)
                top_n = top_n && item.alias != bare(name);
            order += (order.empty() ? "" : ", ") + name + (o.descending ? " DESC" : "");
        }
        std::set<std::string> fetched;
        for (const auto &r : relations)
        {
            if (!Tables().count(r.table) || !fetched.insert(r.table).second)
                continue;
            TableFetch fetch{r.table, r.shards, ""};
            if (r.pushdown)
            {
                std::string where;
                for (const Expression *c : r.conditions)
                    where += (where.empty() ? "" : " AND ") + ("(" + ExpressionToSQL(c) + ")");
                fetch.query = "SELECT *" + (r.name != r.table ? " FROM " + r.table + " " + r.name : " FROM " + r.table) +
                              (where.empty() ? "" : " WHERE " + where);
                if (top_n)
                    fetch.query += " ORDER BY " + order + " LIMIT " + std::to_string(*select.limit + select.offset);
                fetch.query += ";";
            }
            else
            {
                fetch.shards = Tables().at(r.table).kind == "replicated" ? std::vector<uint32_t>{0} : AllShards();
                fetch.query = "SELECT * FROM " + r.table + ";";
            }
            plan.fetches.push_back(fetch);
        }
        // Tables named only inside subqueries: all of their rows
        for (const auto &[table, count] : mentions)
            if (fetched.insert(table).second)
                plan.fetches.push_back({table,
                                        Tables().at(table).kind == "replicated" ? std::vector<uint32_t>{0}
                                                                                    : AllShards(),
                                        "SELECT * FROM " + table + ";"});
        return plan;
    }

    std::string Coordinator::Describe(const SelectPlan &plan) const
    {
        switch (plan.kind)
        {
        case SelectPlan::Kind::SINGLE_SHARD:
            return "Distributed: the whole statement on shard " + std::to_string(plan.shard) + "\n";
        case SelectPlan::Kind::PARTIAL_AGGREGATE:
            return "Distributed: partial aggregates on shards " + List(plan.shards) + ": " + plan.shard_query +
                   " (combined by the coordinator)\n";
        case SelectPlan::Kind::GATHER:
            break;
        }
        std::string text = "Distributed: rows gathered to the coordinator\n";
        for (const auto &fetch : plan.fetches)
            text += "  " + fetch.table + " from shards " + List(fetch.shards) + ": " + fetch.query + "\n";
        return text + "Coordinator:\n";
    }

    ExecutionResult Coordinator::RunPartialAggregate(const SelectPlan &plan, const SelectStatement &select,
                                                     Database &schema)
    {
        // Column names (and checks) from the statement over the empty schema
        Session session(&schema);
        ExecutionResult result = session.Execute(SelectToSQL(select) + ";");
        if (!result.success)
            return result;
        struct Total
        {
            int64_t count = 0, int_sum = 0;
            double float_sum = 0;
            bool has_float = false, any = false;
            std::optional<Value> best;
        };
        std::vector<Total> totals(plan.functions.size());
        for (uint32_t shard : plan.shards)
        {
            ExecutionResult part = ShardCall(shard, "read", plan.shard_query);
            if (!part.success)
                return part;
            const Tuple &row = part.tuples.at(0);
            size_t column = 0;
            for (size_t i = 0; i < plan.functions.size(); ++i)
            {
                Total &t = totals[i];
                const Value &v = row.GetValue(column++);
                switch (plan.functions[i])
                {
                case AggregateFunction::COUNT:
                    t.count += v.GetAsInt();
                    break;
                case AggregateFunction::SUM:
                case AggregateFunction::AVG:
                    if (!v.IsNull())
                    {
                        t.any = true;
                        if (v.GetType() == DataType::FLOAT)
                            t.float_sum += v.GetAsFloat(), t.has_float = true;
                        else if (__builtin_add_overflow(t.int_sum, static_cast<int64_t>(v.GetAsInt()), &t.int_sum))
                            return Failure("Integer overflow");
                    }
                    if (plan.functions[i] == AggregateFunction::AVG)
                        t.count += row.GetValue(column++).GetAsInt();
                    break;
                case AggregateFunction::MIN:
                case AggregateFunction::MAX:
                    if (!v.IsNull() &&
                        (!t.best || (plan.functions[i] == AggregateFunction::MIN ? CompareForSort(v, *t.best) < 0
                                                                                 : CompareForSort(v, *t.best) > 0)))
                        t.best = v;
                    break;
                }
            }
        }
        std::vector<Value> values;
        for (size_t i = 0; i < plan.functions.size(); ++i)
        {
            const Total &t = totals[i];
            switch (plan.functions[i])
            {
            case AggregateFunction::COUNT:
                if (t.count > INT32_MAX)
                    return Failure("Integer overflow");
                values.push_back(Value(static_cast<int32_t>(t.count)));
                break;
            case AggregateFunction::SUM:
                if (!t.any)
                    values.push_back(Value(DataType::INTEGER));
                else if (t.has_float)
                    values.push_back(Value(static_cast<double>(t.int_sum) + t.float_sum));
                else if (t.int_sum < INT32_MIN || t.int_sum > INT32_MAX)
                    return Failure("Integer overflow");
                else
                    values.push_back(Value(static_cast<int32_t>(t.int_sum)));
                break;
            case AggregateFunction::AVG:
                values.push_back(t.count == 0 ? Value(DataType::FLOAT)
                                              : Value((static_cast<double>(t.int_sum) + t.float_sum) /
                                                      static_cast<double>(t.count)));
                break;
            case AggregateFunction::MIN:
            case AggregateFunction::MAX:
                values.push_back(t.best ? *t.best : Value());
                break;
            }
        }
        result.tuples = {Tuple(values)};
        return result;
    }

    ExecutionResult Coordinator::Select(const std::string &sql)
    {
        auto data = SchemaDatabase();
        std::string error;
        auto stmt = ParseOne(sql, &error);
        const SelectStatement *select = nullptr;
        if (stmt && stmt->GetType() == StatementType::SELECT)
            select = static_cast<const SelectStatement *>(stmt.get());
        else if (stmt && stmt->GetType() == StatementType::EXPLAIN_STMT)
            select = static_cast<const ExplainStatement *>(stmt.get())->select.get();
        if (!select)
        {
            Session session(data.get()); // reports the error as a single node does
            return session.Execute(sql);
        }
        const bool explain = stmt->GetType() == StatementType::EXPLAIN_STMT;
        const SelectPlan plan = PlanSelect(*select, sql, *data);

        if (plan.kind == SelectPlan::Kind::SINGLE_SHARD)
        {
            ExecutionResult result = ShardCall(plan.shard, "read", sql);
            if (explain && result.success)
                result.message = Describe(plan) + result.message;
            return result;
        }
        if (plan.kind == SelectPlan::Kind::PARTIAL_AGGREGATE)
        {
            if (explain)
                return Success(Describe(plan));
            return RunPartialAggregate(plan, *select, *data);
        }
        Catalog &catalog = data->GetCatalog();
        for (const auto &fetch : plan.fetches)
        {
            Table *table = catalog.GetTable(fetch.table);
            for (uint32_t shard : explain ? std::vector<uint32_t>{} : fetch.shards)
            {
                ExecutionResult rows = ShardCall(shard, "read", fetch.query);
                if (!rows.success)
                    return rows;
                for (const auto &tuple : rows.tuples)
                {
                    try
                    {
                        catalog.InsertRow(table, tuple);
                    }
                    catch (const std::exception &e)
                    {
                        return Failure("rows of " + fetch.table + " on different shards conflict: " + e.what());
                    }
                }
            }
        }
        Session session(data.get());
        ExecutionResult result = session.Execute(sql);
        if (explain && result.success)
            result.message = Describe(plan) + result.message;
        return result;
    }

    std::unique_ptr<Expression> Coordinator::BindSubqueries(const Expression *expr, std::string *error)
    {
        if (!expr)
            return nullptr;
        return RewriteExpression(
            expr,
            [&](const Expression *node) -> std::unique_ptr<Expression>
            {
                if (node->GetType() != ExpressionType::SUBQUERY || !error->empty())
                    return nullptr;
                const auto *sub = static_cast<const SubqueryExpression *>(node);
                // Run on the whole cluster, against the data as it is before
                // the statement (as a single node evaluates it). A subquery
                // that refers to the outer row does not run on its own.
                ExecutionResult rows = Select(SelectToSQL(*sub->select) + ";");
                if (!rows.success)
                {
                    // It names something only the outer statement has
                    const bool correlated = rows.message.find("Unknown column") != std::string::npos ||
                                            rows.message.find("Unknown table qualifier") != std::string::npos;
                    *error = correlated
                                 ? "correlated subqueries in UPDATE and DELETE are not supported in a cluster (" +
                                       rows.message + ")"
                                 : rows.message;
                    return std::make_unique<LiteralExpression>(Value(DataType::INTEGER));
                }
                switch (sub->kind)
                {
                case SubqueryExpression::Kind::SCALAR:
                    if (rows.tuples.size() > 1)
                        *error = "Scalar subquery returned more than one row";
                    return std::make_unique<LiteralExpression>(rows.tuples.empty() ? Value(DataType::INTEGER)
                                                                                    : rows.tuples[0].GetValue(0));
                case SubqueryExpression::Kind::EXISTS:
                    return std::make_unique<LiteralExpression>(Value(rows.tuples.empty() == sub->negated));
                case SubqueryExpression::Kind::IN:
                {
                    // x IN (no rows) is FALSE, even for a NULL x; NOT IN, TRUE
                    if (rows.tuples.empty())
                        return std::make_unique<LiteralExpression>(Value(sub->negated));
                    std::vector<std::unique_ptr<Expression>> list;
                    for (const auto &tuple : rows.tuples)
                        list.push_back(std::make_unique<LiteralExpression>(tuple.GetValue(0)));
                    return std::make_unique<InListExpression>(BindSubqueries(sub->operand.get(), error),
                                                              std::move(list), sub->negated);
                }
                }
                return nullptr;
            });
    }

    std::string Coordinator::BoundUpdate(const UpdateStatement &update, std::string *error)
    {
        std::string sql = "UPDATE " + update.table + " SET ";
        for (size_t i = 0; i < update.assignments.size(); ++i)
            sql += (i ? ", " : "") + update.assignments[i].first + " = " +
                   ExpressionToSQL(BindSubqueries(update.assignments[i].second.get(), error).get());
        if (update.where)
            sql += " WHERE " + ExpressionToSQL(BindSubqueries(update.where.get(), error).get());
        return sql + ";";
    }

    std::string Coordinator::BoundDelete(const DeleteStatement &del, std::string *error)
    {
        std::string sql = "DELETE FROM " + del.table;
        if (del.where)
            sql += " WHERE " + ExpressionToSQL(BindSubqueries(del.where.get(), error).get());
        return sql + ";";
    }

    std::string Coordinator::Command(const std::string &name, const std::string &argument)
    {
        std::lock_guard<std::mutex> lock(op_mutex_);
        if (!IsLeader())
            return "Error: not the leading coordinator\n";
        try
        {
            if (!was_leader_)
                BecomeLeader();
            if (name == "describe")
                return "Cluster: coordinator " + std::to_string(options_.id) + " (leader), " +
                       std::to_string(view_.nodes.size()) + " storage node(s), " + std::to_string(view_.shards) +
                       " shards x " + std::to_string(view_.rf) + " replicas, epoch " + std::to_string(view_.epoch) +
                       (view_.formed ? "" : " (forming)") + "\n";
            if (name == "cluster")
            {
                std::istringstream words(argument);
                std::string verb, node;
                words >> verb >> node;
                if (verb.empty() || verb == "status")
                    return ClusterStatus();
                if (verb == "remove" && !node.empty())
                    return RemoveNode(node);
                if (verb == "add" && !node.empty())
                {
                    // Undo a removal: the name may join again (e.g. a
                    // StatefulSet scaled down and back up reuses node names)
                    for (const auto &[id, info] : view_.nodes)
                        if (info.name == node)
                        {
                            if (!view_.removed.count(id))
                                return node + " is already a member\n";
                            MetaWrite("DELETE FROM node_state WHERE id = " + std::to_string(id) + ";");
                            Load();
                            return node + " may join again; it gets replicas once it announces itself\n";
                        }
                    return "Error: no storage node named " + node + " (a new node joins by itself)\n";
                }
                return "Usage: .cluster [status | remove NODE | add NODE]\n";
            }
            if (name == "tables")
            {
                if (Tables().empty())
                    return "No tables.\n";
                auto schema = SchemaDatabase();
                std::string out;
                for (const auto &[table_name, info] : Tables())
                {
                    out += table_name + " [" +
                           (info.kind == "hash"         ? "by " + info.shard_column
                            : info.kind == "replicated" ? std::string("replicated")
                                                        : std::string("by row")) +
                           "]: ";
                    const Table *table = schema->GetCatalog().GetTable(table_name);
                    const auto &columns = table->GetSchema().GetColumns();
                    for (size_t i = 0; i < columns.size(); ++i)
                        out += (i ? ", " : "") + columns[i].name + " " + DataTypeName(columns[i].type);
                    out += "\n";
                }
                return out;
            }
            if (name == "schema")
            {
                std::string out;
                if (!argument.empty() && !Tables().count(argument))
                    return "Error: no table named " + argument + "\n";
                for (const auto &[table_name, info] : Tables())
                    if (argument.empty() || argument == table_name)
                        out += info.definition + "\n";
                return out;
            }
            return "Error: ." + name + " is not available on a cluster\n";
        }
        catch (const std::exception &e)
        {
            was_leader_ = false;
            return std::string("Error: ") + e.what() + "\n";
        }
    }

} // namespace sql::cluster
