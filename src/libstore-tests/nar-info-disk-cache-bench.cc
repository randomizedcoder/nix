#include <benchmark/benchmark.h>

#include "nix/store/globals.hh"
#include "nix/store/nar-info-disk-cache.hh"
#include "nix/store/nar-info.hh"
#include "nix/store/sqlite.hh"
#include "nix/util/file-system.hh"
#include "nix/util/hash.hh"

#include <sqlite3.h>

namespace nix {

enum class IndexMode { WithIndex, WithoutIndex };

static const char * indexModeLabel(IndexMode mode)
{
    return mode == IndexMode::WithIndex ? "with_index" : "without_index";
}

static void ensureIndexMode(const std::filesystem::path & dbPath, IndexMode mode)
{
    SQLite db(dbPath, {.useWAL = settings.useSQLiteWAL});
    if (mode == IndexMode::WithIndex) {
        db.exec("create index if not exists IndexNARsTimestamp on NARs(timestamp);");
    } else {
        db.exec("drop index if exists IndexNARsTimestamp;");
    }
}

static int
populateCache(ref<NarInfoDiskCache> & cache, const std::string & uri, const std::string & storeDir, int count)
{
    auto cacheId = cache->createCache(uri, storeDir, true, 10);

    for (int i = 0; i < count; ++i) {
        auto path = StorePath::random(fmt("bench-nar-%d", i));
        auto hashPart = std::string(path.hashPart());
        auto narInfo = std::make_shared<NarInfo>(storeDir, path, Hash::dummy);
        narInfo->narSize = 1024;
        cache->upsertNarInfo(uri, hashPart, narInfo);
    }

    return cacheId;
}

// ---------------------------------------------------------------------------
// Benchmark A: Write (Upsert) — measure insert performance with/without index
// ---------------------------------------------------------------------------

static void BM_NarCacheWrite(benchmark::State & state)
{
    const int count = state.range(0);
    const auto mode = static_cast<IndexMode>(state.range(1));

    const std::string storeDir = "/nix/store";
    const std::string uri = "https://bench-write.example.com";

    for (auto _ : state) {
        state.PauseTiming();

        auto tmpDir = createTempDir();
        AutoDelete delTmpDir(tmpDir);
        auto dbPath = std::filesystem::path(tmpDir) / "bench.sqlite";

        auto cache = NarInfoDiskCache::getTest(
            settings.getNarInfoDiskCacheSettings(), {.useWAL = settings.useSQLiteWAL}, dbPath);
        cache->createCache(uri, storeDir, true, 10);

        ensureIndexMode(dbPath, mode);

        std::vector<std::pair<std::string, std::shared_ptr<NarInfo>>> entries;
        entries.reserve(count);
        for (int i = 0; i < count; ++i) {
            auto path = StorePath::random(fmt("bench-write-%d", i));
            auto hashPart = std::string(path.hashPart());
            auto narInfo = std::make_shared<NarInfo>(storeDir, path, Hash::dummy);
            narInfo->narSize = 1024;
            entries.emplace_back(std::move(hashPart), std::move(narInfo));
        }

        state.ResumeTiming();

        for (auto & [hashPart, narInfo] : entries) {
            cache->upsertNarInfo(uri, hashPart, narInfo);
        }
    }

    state.SetItemsProcessed(state.iterations() * count);
    state.SetLabel(indexModeLabel(mode));
}

// ---------------------------------------------------------------------------
// Benchmark B: Read (Lookup) — measure lookup performance with/without index
// ---------------------------------------------------------------------------

static void BM_NarCacheRead(benchmark::State & state)
{
    const int count = state.range(0);
    const auto mode = static_cast<IndexMode>(state.range(1));

    const std::string storeDir = "/nix/store";
    const std::string uri = "https://bench-read.example.com";

    auto tmpDir = createTempDir();
    AutoDelete delTmpDir(tmpDir);
    auto dbPath = std::filesystem::path(tmpDir) / "bench.sqlite";

    std::vector<std::string> hashParts;
    hashParts.reserve(count);

    {
        auto cache = NarInfoDiskCache::getTest(
            settings.getNarInfoDiskCacheSettings(), {.useWAL = settings.useSQLiteWAL}, dbPath);
        cache->createCache(uri, storeDir, true, 10);

        for (int i = 0; i < count; ++i) {
            auto path = StorePath::random(fmt("bench-read-%d", i));
            auto hashPart = std::string(path.hashPart());
            auto narInfo = std::make_shared<NarInfo>(storeDir, path, Hash::dummy);
            narInfo->narSize = 1024;
            cache->upsertNarInfo(uri, hashPart, narInfo);
            hashParts.push_back(hashPart);
        }
    }

    ensureIndexMode(dbPath, mode);

    for (auto _ : state) {
        state.PauseTiming();

        auto cache = NarInfoDiskCache::getTest(
            settings.getNarInfoDiskCacheSettings(), {.useWAL = settings.useSQLiteWAL}, dbPath);
        cache->createCache(uri, storeDir, true, 10);

        state.ResumeTiming();

        for (auto & hashPart : hashParts) {
            auto result = cache->lookupNarInfo(uri, hashPart);
            benchmark::DoNotOptimize(result);
        }
    }

    state.SetItemsProcessed(state.iterations() * count);
    state.SetLabel(indexModeLabel(mode));
}

// ---------------------------------------------------------------------------
// Benchmark C: Purge (Delete) — measure purge query performance with/without
// index at varying purge percentages.
//
// Args: {total_entries, index_mode, purge_percent}
// ---------------------------------------------------------------------------

static void BM_NarCachePurge(benchmark::State & state)
{
    const int count = state.range(0);
    const auto mode = static_cast<IndexMode>(state.range(1));
    const int purgePercent = state.range(2);

    const std::string storeDir = "/nix/store";
    const std::string uri = "https://bench-purge.example.com";

    for (auto _ : state) {
        state.PauseTiming();

        auto tmpDir = createTempDir();
        AutoDelete delTmpDir(tmpDir);
        auto dbPath = std::filesystem::path(tmpDir) / "bench.sqlite";

        {
            auto cache = NarInfoDiskCache::getTest(
                settings.getNarInfoDiskCacheSettings(), {.useWAL = settings.useSQLiteWAL}, dbPath);
            populateCache(cache, uri, storeDir, count);
        }

        // Backdate only purgePercent% of entries
        {
            SQLite db(dbPath, {.useWAL = settings.useSQLiteWAL});
            auto expiredCount = count * purgePercent / 100;
            SQLiteStmt(
                db,
                "update NARs set timestamp = cast(strftime('%s', 'now') as integer) - 365 * 24 * 3600 "
                "where rowid in (select rowid from NARs limit ?)")
                .use()(static_cast<int64_t>(expiredCount))
                .exec();
        }

        ensureIndexMode(dbPath, mode);

        SQLite db(dbPath, {.useWAL = settings.useSQLiteWAL});
        auto now = time(nullptr);

        state.ResumeTiming();

        SQLiteStmt(db, "delete from NARs where ((present = 0 and timestamp < ?) or (present = 1 and timestamp < ?))")
            .use()(now - 3600)(now - 30 * 24 * 3600)
            .exec();

        state.PauseTiming();
        auto deleted = sqlite3_changes(db);
        benchmark::DoNotOptimize(deleted);
        state.ResumeTiming();
    }

    state.SetItemsProcessed(state.iterations() * count);
    state.SetLabel(fmt("%d%% purged, %s", purgePercent, indexModeLabel(mode)));
}

// ---------------------------------------------------------------------------
// Registration: generate all {size x index_mode} and
//               {size x index_mode x purge_percent} combinations.
// ---------------------------------------------------------------------------

static const std::initializer_list<int64_t> defaultSizes = {1000, 10000, 50000, 100000};
static const std::initializer_list<int> purgePercentages = {5, 10, 20, 30, 40, 50};

static int registerBenchmarks = []() {
    for (auto size : defaultSizes) {
        for (auto idx : {IndexMode::WithoutIndex, IndexMode::WithIndex}) {
            benchmark::RegisterBenchmark("BM_NarCacheWrite", BM_NarCacheWrite)
                ->Args({size, static_cast<int>(idx)})
                ->Unit(benchmark::kMillisecond);

            benchmark::RegisterBenchmark("BM_NarCacheRead", BM_NarCacheRead)
                ->Args({size, static_cast<int>(idx)})
                ->Unit(benchmark::kMillisecond);
        }
    }

    for (auto pct : purgePercentages) {
        for (auto idx : {IndexMode::WithoutIndex, IndexMode::WithIndex}) {
            benchmark::RegisterBenchmark("BM_NarCachePurge", BM_NarCachePurge)
                ->Args({100000, static_cast<int>(idx), pct})
                ->Unit(benchmark::kMillisecond);
        }
    }

    return 0;
}();

} // namespace nix
