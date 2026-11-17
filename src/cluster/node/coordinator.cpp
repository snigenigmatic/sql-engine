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
#include <set>

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

            ExecutionResult Execute(const std::string &sql) override
            {
                if (coordinator_->IsLeader())
                    return coordinator_->Execute(sql);
                try
                {
                    if (auto *leader = Leader())
                        return leader->Execute(sql);
                }
                catch (const std::exception &e)
                {
                    forward_.reset();
                    return Failure(std::string("lost the leading coordinator: ") + e.what());
                }
                return Failure("no coordinator is leading right now; try again shortly");
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

            bool InTransaction() override { return false; }

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

    ExecutionResult Coordinator::MetaWrite(const std::string &sql)
    {
        ExecutionResult result = metadata_->Write(sql);
        if (!result.success && IsUnavailable(result.message))
            throw std::runtime_error("metadata unavailable: " + result.message);
        return result;
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
        if (MetaRead("SELECT COUNT(*) FROM cluster;").tuples.at(0).GetValue(0).GetAsInt() == 0)
            MetaWrite("INSERT INTO cluster VALUES (" + std::to_string(options_.shard_count) + ", " +
                      std::to_string(options_.replication_factor) + ", 0, FALSE);");
        // Fence off the previous leader: storage nodes refuse older epochs
        MetaWrite("UPDATE cluster SET epoch = epoch + 1;");
        connections_.clear();
        shard_leaders_.clear();
        ensured_.clear();
        last_full_ensure_ = {};
        Load();
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
        try
        {
            auto connection = std::make_unique<net::RemoteConnection>(info->second.host, info->second.port);
            return (connections_[node] = std::move(connection)).get();
        }
        catch (const std::exception &)
        {
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

    ExecutionResult Coordinator::ShardCall(uint32_t shard, const std::string &method, const std::string &sql)
    {
        const auto deadline = Clock::now() + std::chrono::milliseconds(options_.election_timeout_ms * 6 + 2000);
        std::string last_error = "no replica of shard " + std::to_string(shard) + " answered";
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
                    result = connection->Call(method, {std::to_string(view_.epoch), std::to_string(shard), sql});
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
        return db;
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
        return Success(std::to_string(id));
    }

    ExecutionResult Coordinator::Execute(const std::string &sql)
    {
        std::lock_guard<std::mutex> lock(op_mutex_);
        if (!IsLeader())
            return Failure("not the leader (leader: " + std::to_string(metadata_ ? metadata_->LeaderId() : -1) + ")");
        try
        {
            if (!was_leader_)
                BecomeLeader();
            std::string error;
            auto stmt = ParseOne(sql, &error);
            if (!stmt)
                return Failure(error);
            switch (stmt->GetType())
            {
            case StatementType::CREATE_TABLE:
            case StatementType::DROP_TABLE:
            case StatementType::CREATE_INDEX:
                return Ddl(sql);
            case StatementType::TRANSACTION_STMT:
                return Failure("transactions across the cluster are not supported yet");
            default:
                break;
            }
            if (!view_.formed)
                return Failure("the cluster is still forming: " + std::to_string(view_.nodes.size()) + " of " +
                               std::to_string(view_.rf) + " storage nodes have joined");
            switch (stmt->GetType())
            {
            case StatementType::INSERT:
                return Insert(sql);
            case StatementType::UPDATE_STMT:
            {
                const auto *update = static_cast<const UpdateStatement *>(stmt.get());
                auto table = view_.tables.find(update->table);
                for (const auto &[column, expr] : update->assignments)
                {
                    if (ContainsSubquery(expr.get()))
                        return Failure("subqueries in UPDATE are not supported in a cluster yet");
                    if (table != view_.tables.end() && table->second.kind == "hash" &&
                        column == table->second.shard_column)
                        return Failure("cannot change the shard key " + column + " of " + update->table);
                }
                if (ContainsSubquery(update->where.get()))
                    return Failure("subqueries in UPDATE are not supported in a cluster yet");
                return Broadcast(sql, update->table);
            }
            case StatementType::DELETE_STMT:
            {
                const auto *del = static_cast<const DeleteStatement *>(stmt.get());
                if (ContainsSubquery(del->where.get()))
                    return Failure("subqueries in DELETE are not supported in a cluster yet");
                return Broadcast(sql, del->table);
            }
            case StatementType::ANALYZE_STMT:
            {
                ExecutionResult first;
                for (uint32_t shard : AllShards())
                {
                    ExecutionResult result = ShardCall(shard, "write", sql);
                    if (!result.success)
                        return result;
                    if (shard == 0)
                        first = result;
                }
                return first;
            }
            default: // SELECT, EXPLAIN
                return Select(sql);
            }
        }
        catch (const std::exception &e)
        {
            was_leader_ = false;
            return Failure(e.what());
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
        std::string error;
        auto stmt = ParseOne(sql, &error);
        if (sql.size() > 1900)
            return Failure("statement too long for the cluster's schema log");
        const int64_t seq = view_.ddl.empty() ? 1 : view_.ddl.back().first + 1;
        if (stmt->GetType() == StatementType::CREATE_TABLE)
        {
            const auto *create = static_cast<const CreateTableStatement *>(stmt.get());
            std::string kind = "row", column;
            if (create->distribution == TableDistribution::REPLICATED)
                kind = "replicated";
            else if (create->distribution == TableDistribution::HASH)
                kind = "hash", column = create->shard_column;
            else
            {
                for (const auto &col : create->columns)
                    if (col.primary_key)
                        kind = "hash", column = col.name;
            }
            MetaWrite("INSERT INTO tables VALUES (" + Quote(create->table) + ", " + Quote(sql) + ", " + Quote(kind) +
                      ", " + Quote(column) + ");");
        }
        else if (stmt->GetType() == StatementType::DROP_TABLE)
            MetaWrite("DELETE FROM tables WHERE name = " +
                      Quote(static_cast<const DropTableStatement *>(stmt.get())->table) + ";");
        MetaWrite("INSERT INTO ddl VALUES (" + std::to_string(seq) + ", " + Quote(sql) + ");");
        Load();
        if (view_.formed)
            CatchUpDdl();
        return result;
    }

    ExecutionResult Coordinator::Insert(const std::string &sql)
    {
        std::string error;
        auto stmt = ParseOne(sql, &error);
        const auto *insert = static_cast<const InsertStatement *>(stmt.get());
        auto table = view_.tables.find(insert->table);
        if (table == view_.tables.end())
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
        long long inserted = 0;
        for (const auto &[shard, rows] : per_shard)
        {
            ExecutionResult result =
                ShardCall(shard, "write", "INSERT INTO " + insert->table + " (" + column_list + ") VALUES " + rows + ";");
            if (!result.success)
                return result; // rows sent to other shards stay: no cross-shard atomicity before 2PC (D6)
            if (table->second.kind != "replicated" || shard == per_shard.begin()->first)
                inserted += Count(result.message);
        }
        return Success(std::to_string(inserted) + " row(s) inserted.");
    }

    ExecutionResult Coordinator::Broadcast(const std::string &sql, const std::string &table_name)
    {
        auto table = view_.tables.find(table_name);
        if (table == view_.tables.end())
            return Failure("Table not found: " + table_name);
        auto schema = SchemaDatabase();
        Session check(schema.get());
        ExecutionResult checked = check.Execute(sql); // errors as on a single node
        if (!checked.success)
            return checked;
        long long count = 0;
        std::string verb;
        for (uint32_t shard : AllShards())
        {
            ExecutionResult result = ShardCall(shard, "write", sql);
            if (!result.success)
                return result;
            if (table->second.kind != "replicated" || shard == 0)
                count += Count(result.message);
            verb = result.message.substr(result.message.find(' ') + 1);
        }
        return Success(std::to_string(count) + " " + verb);
    }

    ExecutionResult Coordinator::Select(const std::string &sql)
    {
        auto data = SchemaDatabase();
        // Every table the statement names (any identifier that is one)
        std::set<std::string> names;
        Lexer lexer(sql);
        for (Token token = lexer.NextToken(); token.type != TokenType::END_OF_FILE; token = lexer.NextToken())
            if (token.type == TokenType::IDENTIFIER && view_.tables.count(token.value))
                names.insert(token.value);
        Catalog &catalog = data->GetCatalog();
        for (const auto &name : names)
        {
            const TableInfo &info = view_.tables.at(name);
            Table *table = catalog.GetTable(name);
            const std::vector<uint32_t> sources =
                info.kind == "replicated" ? std::vector<uint32_t>{0} : AllShards();
            for (uint32_t shard : sources)
            {
                ExecutionResult rows = ShardCall(shard, "read", "SELECT * FROM " + name + ";");
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
                        return Failure("rows of " + name + " on different shards conflict: " + e.what());
                    }
                }
            }
        }
        Session session(data.get());
        return session.Execute(sql);
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
            if (name == "tables")
            {
                if (view_.tables.empty())
                    return "No tables.\n";
                auto schema = SchemaDatabase();
                std::string out;
                for (const auto &[table_name, info] : view_.tables)
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
                if (!argument.empty() && !view_.tables.count(argument))
                    return "Error: no table named " + argument + "\n";
                for (const auto &[table_name, info] : view_.tables)
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
