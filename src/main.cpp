#include <cstdlib>
#include <iostream>
#include <string>
#include "common/value.h"
#include "common/schema.h"
#include "common/tuple.h"
#include "lexer/lexer.h"
#include "parser/parser.h"
#include "catalog/catalog.h"
#include "execution/executor.h"
#include "catalog/database.h"
#include "catalog/legacy_import.h"
#include "session/session.h"
#include "cli/local_connection.h"
#include "cli/shell.h"
#include "net/remote_connection.h"
#include <memory>
#include <sys/stat.h>
#include <unistd.h>

std::unique_ptr<sql::Database> g_db;
std::unique_ptr<sql::Session> g_session;

void PrintBanner(const std::string &path)
{
    std::cout << "========================================\n";
    std::cout << "  SQL Engine v0.3.0\n";
    std::cout << "  Database: " << path << "\n";
    std::cout << "  Type .help for help, .quit to exit\n";
    std::cout << "========================================\n\n";
}

// Read lines from standard input into the shell until it quits or input ends
void RunShell(sql::Shell &shell, bool interactive)
{
    std::string line;
    while (true)
    {
        if (interactive)
            std::cout << shell.Prompt() << std::flush;
        if (!std::getline(std::cin, line))
        {
            if (interactive)
                std::cout << "\n";
            break;
        }
        if (!shell.HandleLine(line))
            break;
    }
    if (interactive)
        std::cout << "Goodbye!\n";
}

// Shell over a database served by sqlnode at host:port
int RunRemote(const std::string &address)
{
    const size_t colon = address.rfind(':');
    char *end = nullptr;
    const long port = colon == std::string::npos ? -1 : std::strtol(address.c_str() + colon + 1, &end, 10);
    if (colon == std::string::npos || colon == 0 || *end != '\0' || port <= 0 || port > 65535)
    {
        std::cerr << "Error: expected --connect HOST:PORT, got " << address << "\n";
        return 1;
    }
    std::unique_ptr<sql::net::RemoteConnection> connection;
    try
    {
        connection = std::make_unique<sql::net::RemoteConnection>(address.substr(0, colon),
                                                                  static_cast<uint16_t>(port));
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    sql::Shell shell(connection.get(), std::cout);
    const bool interactive = isatty(STDIN_FILENO);
    if (interactive)
        PrintBanner(address + " (remote)");
    RunShell(shell, interactive);
    // Disconnecting rolls back a transaction left open on the server
    if (connection->InTransaction())
        std::cout << "Rolling back the open transaction.\n";
    return 0;
}

// True if the path names an existing, non-empty file
bool DatabaseFileHasContent(const std::string &path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0 && st.st_size > 0;
}

int main(int argc, char **argv)
{
    std::string path = "sqlengine.db";
    if (argc > 1)
    {
        std::string arg = argv[1];
        if (arg == "-h" || arg == "--help")
        {
            std::cout << "Usage: " << argv[0] << " [database-file]   (default: sqlengine.db)\n"
                      << "       " << argv[0] << " --connect HOST:PORT   (a database served by sqlnode)\n"
                      << "Reads SQL and .commands from standard input; pipe a script to run it.\n";
            return 0;
        }
        if (arg == "--connect")
        {
            if (argc < 3)
            {
                std::cerr << "Error: --connect needs HOST:PORT\n";
                return 1;
            }
            return RunRemote(argv[2]);
        }
        path = arg;
    }

    // One-time migration of text snapshots written by earlier versions. It
    // only runs when the database file does not exist yet, and is all or
    // nothing: a failed import leaves no database behind and is retried on
    // the next launch.
    std::string error;
    sql::LegacyImporter legacy;
    bool imported = false;
    if (!DatabaseFileHasContent(path) && legacy.HasSnapshot())
    {
        size_t table_count = 0;
        if (!sql::MigrateLegacySnapshot(legacy.GetDirectory(), path, &table_count, &error))
        {
            std::cerr << "Error: could not import legacy snapshot from " << legacy.GetDirectory()
                      << "/: " << error << "\n"
                      << "No database was created. Fix or move the snapshot aside, then retry.\n";
            return 1;
        }
        std::cout << "Imported " << table_count << " table(s) from legacy snapshot "
                  << legacy.GetDirectory() << "/\n";
        imported = true;
    }

    g_db = sql::Database::Open(path, &error);
    if (!g_db)
    {
        std::cerr << "Error: " << error << "\n";
        return 1;
    }

    if (!imported && isatty(STDIN_FILENO))
    {
        auto tables = g_db->GetCatalog().GetTableNames();
        if (!tables.empty())
            std::cout << "Loaded " << tables.size() << " table(s) from " << path << ".\n";
    }

    g_session = std::make_unique<sql::Session>(g_db.get());
    sql::LocalConnection connection(g_db.get(), g_session.get());
    sql::Shell shell(&connection, std::cout);

    // Piped input (sqlengine db < script.sql) gets no banner or prompts
    const bool interactive = isatty(STDIN_FILENO);
    if (interactive)
        PrintBanner(path);

    RunShell(shell, interactive);

    if (g_session->InTransaction())
        std::cout << "Rolling back the open transaction.\n";
    g_session.reset(); // rolls back an open transaction
    g_db.reset();      // checkpoints and removes the write-ahead log
    return 0;
}
