#include <gtest/gtest.h>

#include "nix/store/sqlite.hh"
#include "nix/store/globals.hh"
#include "nix/util/file-system.hh"

#include <sqlite3.h>
#include <thread>
#include <atomic>

namespace nix {

/* ---------- helpers --------------------------------------------------- */

/// Create a temporary SQLite database and return its path.
/// The AutoDelete handle keeps the directory alive.
static std::pair<std::filesystem::path, AutoDelete> makeTempDb()
{
    auto tmpDir = createTempDir();
    AutoDelete del(tmpDir);
    auto dbPath = tmpDir / "test.sqlite";
    return {dbPath, std::move(del)};
}

/// Trigger a real SQLITE_BUSY on db2 by holding an exclusive lock on db1.
/// Returns after putting db2 into a BUSY error state.
static void triggerBusy(sqlite3 * db1, sqlite3 * db2)
{
    sqlite3_busy_timeout(db2, 0);
    sqlite3_exec(db1, "BEGIN EXCLUSIVE", nullptr, nullptr, nullptr);
    sqlite3_exec(db1, "INSERT INTO t VALUES (999)", nullptr, nullptr, nullptr);
    // db2 is now in BUSY state after this fails:
    sqlite3_exec(db2, "INSERT INTO t VALUES (998)", nullptr, nullptr, nullptr);
}

/// Create a SQLiteBusy exception through the real code path by triggering
/// actual contention. This avoids accessing the protected `err` member.
static SQLiteBusy makeBusyException()
{
    auto [dbPath, del] = makeTempDb();
    SQLite db1(dbPath, {.useWAL = false});
    db1.exec("CREATE TABLE t (id INTEGER PRIMARY KEY)");
    SQLite db2(dbPath, {.useWAL = false});
    triggerBusy(db1, db2);

    try {
        SQLiteError::throw_(db2, "test busy");
    } catch (SQLiteBusy & e) {
        sqlite3_exec(db1, "ROLLBACK", nullptr, nullptr, nullptr);
        return e;
    }
    // unreachable, but needed to satisfy the compiler
    sqlite3_exec(db1, "ROLLBACK", nullptr, nullptr, nullptr);
    std::abort();
}

/* ---------- SQLite open / close --------------------------------------- */

TEST(SQLite, open_and_close)
{
    auto [dbPath, del] = makeTempDb();
    {
        SQLite db(dbPath, {.useWAL = false});
        ASSERT_NE(db.db, nullptr);
    }
    // destructor should close cleanly
}

TEST(SQLite, exec_creates_table)
{
    auto [dbPath, del] = makeTempDb();
    SQLite db(dbPath, {.useWAL = false});
    ASSERT_NO_THROW(db.exec("CREATE TABLE t (id INTEGER PRIMARY KEY, val TEXT)"));
}

/* ---------- SQLiteTxn ------------------------------------------------- */

TEST(SQLiteTxn, commit_persists_data)
{
    auto [dbPath, del] = makeTempDb();
    SQLite db(dbPath, {.useWAL = false});
    db.exec("CREATE TABLE t (id INTEGER PRIMARY KEY)");
    {
        SQLiteTxn txn(db);
        db.exec("INSERT INTO t VALUES (1)");
        txn.commit();
    }
    // Verify row persisted
    SQLiteStmt stmt(db, "SELECT COUNT(*) FROM t");
    auto q = stmt.use();
    ASSERT_TRUE(q.next());
    EXPECT_EQ(q.getInt(0), 1);
}

TEST(SQLiteTxn, rollback_on_destroy)
{
    auto [dbPath, del] = makeTempDb();
    SQLite db(dbPath, {.useWAL = false});
    db.exec("CREATE TABLE t (id INTEGER PRIMARY KEY)");
    {
        SQLiteTxn txn(db);
        db.exec("INSERT INTO t VALUES (1)");
        // no commit — destructor should rollback
    }
    SQLiteStmt stmt(db, "SELECT COUNT(*) FROM t");
    auto q = stmt.use();
    ASSERT_TRUE(q.next());
    EXPECT_EQ(q.getInt(0), 0);
}

/* ---------- SQLiteError / SQLiteBusy ---------------------------------- */

TEST(SQLiteError, throw_on_bad_sql)
{
    auto [dbPath, del] = makeTempDb();
    SQLite db(dbPath, {.useWAL = false});
    EXPECT_THROW(db.exec("NOT VALID SQL"), SQLiteError);
}

TEST(SQLiteError, throw_busy_is_SQLiteBusy)
{
    auto [dbPath, del] = makeTempDb();
    SQLite db1(dbPath, {.useWAL = false});
    db1.exec("CREATE TABLE t (id INTEGER PRIMARY KEY)");
    SQLite db2(dbPath, {.useWAL = false});

    triggerBusy(db1, db2);

    EXPECT_THROW(SQLiteError::throw_(db2, "test busy"), SQLiteBusy);

    sqlite3_exec(db1, "ROLLBACK", nullptr, nullptr, nullptr);
}

TEST(SQLiteError, busy_exception_contains_path)
{
    auto [dbPath, del] = makeTempDb();
    SQLite db1(dbPath, {.useWAL = false});
    db1.exec("CREATE TABLE t (id INTEGER PRIMARY KEY)");
    SQLite db2(dbPath, {.useWAL = false});

    triggerBusy(db1, db2);

    try {
        SQLiteError::throw_(db2, "test busy");
        FAIL() << "Expected SQLiteBusy";
    } catch (const SQLiteBusy & e) {
        auto msg = e.info().msg.str();
        EXPECT_NE(msg.find("is busy"), std::string::npos)
            << "Error message should contain 'is busy', got: " << msg;
    }

    sqlite3_exec(db1, "ROLLBACK", nullptr, nullptr, nullptr);
}

TEST(SQLiteError, busy_exception_has_correct_errNo)
{
    auto [dbPath, del] = makeTempDb();
    SQLite db1(dbPath, {.useWAL = false});
    db1.exec("CREATE TABLE t (id INTEGER PRIMARY KEY)");
    SQLite db2(dbPath, {.useWAL = false});

    triggerBusy(db1, db2);

    try {
        SQLiteError::throw_(db2, "test busy");
        FAIL() << "Expected SQLiteBusy";
    } catch (const SQLiteBusy & e) {
        EXPECT_EQ(e.errNo, SQLITE_BUSY);
    }

    sqlite3_exec(db1, "ROLLBACK", nullptr, nullptr, nullptr);
}

/* ---------- retrySQLite ----------------------------------------------- */

TEST(retrySQLite, immediate_success)
{
    int calls = 0;
    auto result = retrySQLite<int>([&]() {
        calls++;
        return 42;
    });
    EXPECT_EQ(result, 42);
    EXPECT_EQ(calls, 1);
}

TEST(retrySQLite, retries_on_SQLiteBusy)
{
    // Create a real SQLiteBusy exception to reuse
    auto busyExc = makeBusyException();

    int attempts = 0;
    auto result = retrySQLite<int>([&]() -> int {
        attempts++;
        if (attempts < 3)
            throw busyExc;
        return 99;
    });
    EXPECT_EQ(result, 99);
    EXPECT_EQ(attempts, 3);
}

TEST(retrySQLite, void_return_type)
{
    auto busyExc = makeBusyException();

    int attempts = 0;
    ASSERT_NO_THROW(retrySQLite<void>([&]() {
        attempts++;
        if (attempts < 2)
            throw busyExc;
    }));
    EXPECT_EQ(attempts, 2);
}

TEST(retrySQLite, propagates_non_busy_errors)
{
    EXPECT_THROW(
        retrySQLite<void>([]() {
            throw SQLiteError("", "some error", SQLITE_ERROR, SQLITE_ERROR, -1, HintFmt("non-busy error"));
        }),
        SQLiteError);
}

/* ---------- handleSQLiteBusy ------------------------------------------ */

TEST(handleSQLiteBusy, does_not_crash)
{
    auto busyExc = makeBusyException();
    time_t nextWarning = 0;
    ASSERT_NO_THROW(handleSQLiteBusy(busyExc, nextWarning));
}

TEST(handleSQLiteBusy, suppresses_frequent_warnings)
{
    // Without NIX_DEBUG_LOCK, warnings are throttled to every 10s.
    // Calling twice in quick succession should not crash, and
    // nextWarning should be advanced on the first call.
    auto busyExc = makeBusyException();

    time_t nextWarning = 0;
    handleSQLiteBusy(busyExc, nextWarning);
    // nextWarning should have been advanced to ~now+10
    EXPECT_GT(nextWarning, 0);
    time_t saved = nextWarning;

    // Second call — nextWarning should not change because it's still in the future
    handleSQLiteBusy(busyExc, nextWarning);
    EXPECT_EQ(nextWarning, saved);
}

/* ---------- real contention via retrySQLite --------------------------- */

TEST(retrySQLite, real_contention_resolves)
{
    auto [dbPath, del] = makeTempDb();

    SQLite db1(dbPath, {.useWAL = false});
    db1.exec("CREATE TABLE t (id INTEGER PRIMARY KEY, val TEXT)");
    db1.exec("INSERT INTO t VALUES (1, 'initial')");

    SQLite db2(dbPath, {.useWAL = false});
    // Short busy timeout so it fails fast on each attempt
    sqlite3_busy_timeout(db2, 50);

    // db1 holds an exclusive lock
    sqlite3_exec(db1, "BEGIN EXCLUSIVE", nullptr, nullptr, nullptr);

    std::atomic<bool> released{false};

    // Release the lock from a background thread after a short delay
    std::thread releaser([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds{200});
        sqlite3_exec(db1, "COMMIT", nullptr, nullptr, nullptr);
        released = true;
    });

    // db2 should retry and eventually succeed once db1 releases
    ASSERT_NO_THROW(retrySQLite<void>([&]() {
        if (sqlite3_exec(db2, "INSERT INTO t VALUES (2, 'retried')", nullptr, nullptr, nullptr) != SQLITE_OK)
            SQLiteError::throw_(db2, "contention test");
    }));

    releaser.join();
    EXPECT_TRUE(released);

    // Verify both rows exist
    SQLiteStmt stmt(db1, "SELECT COUNT(*) FROM t");
    auto q = stmt.use();
    ASSERT_TRUE(q.next());
    EXPECT_EQ(q.getInt(0), 2);
}

/* ---------- SQLiteStmt ------------------------------------------------ */

TEST(SQLiteStmt, bind_and_query)
{
    auto [dbPath, del] = makeTempDb();
    SQLite db(dbPath, {.useWAL = false});
    db.exec("CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT)");

    SQLiteStmt insert(db, "INSERT INTO t (id, name) VALUES (?, ?)");
    insert.use()(1)("hello").exec();
    insert.use()(2)("world").exec();

    SQLiteStmt select(db, "SELECT name FROM t ORDER BY id");
    auto q = select.use();
    ASSERT_TRUE(q.next());
    EXPECT_EQ(q.getStr(0), "hello");
    ASSERT_TRUE(q.next());
    EXPECT_EQ(q.getStr(0), "world");
    ASSERT_FALSE(q.next());
}

TEST(SQLiteStmt, getLastInsertedRowId)
{
    auto [dbPath, del] = makeTempDb();
    SQLite db(dbPath, {.useWAL = false});
    db.exec("CREATE TABLE t (id INTEGER PRIMARY KEY AUTOINCREMENT, val TEXT)");

    SQLiteStmt insert(db, "INSERT INTO t (val) VALUES (?)");
    insert.use()("first").exec();
    EXPECT_EQ(db.getLastInsertedRowId(), 1u);

    insert.use()("second").exec();
    EXPECT_EQ(db.getLastInsertedRowId(), 2u);
}

/* ---------- isCache mode ---------------------------------------------- */

TEST(SQLite, isCache_sets_wal_mode)
{
    auto [dbPath, del] = makeTempDb();
    SQLite db(dbPath, {.useWAL = true});
    db.isCache();

    SQLiteStmt stmt(db, "PRAGMA journal_mode");
    auto q = stmt.use();
    ASSERT_TRUE(q.next());
    EXPECT_EQ(q.getStr(0), "wal");
}

} // namespace nix
