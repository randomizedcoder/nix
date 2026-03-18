#include <benchmark/benchmark.h>

#include "nix/store/canonicalizing-source-accessor.hh"
#include "nix/store/posix-fs-canonicalise.hh"
#include "nix/store/path-references.hh"
#include "nix/util/archive.hh"
#include "nix/util/file-system.hh"
#include "nix/util/hash.hh"
#include "nix/util/posix-source-accessor.hh"
#include "nix/util/serialise.hh"

#ifndef _WIN32

#  include <filesystem>
#  include <fstream>

using namespace nix;

static const StringSet benchEmptyAcls{};

#  define BENCH_CANON_OPTIONS {.uidRange = std::nullopt, NIX_WHEN_SUPPORT_ACLS(benchEmptyAcls)}

/// Make a canonicalized tree deletable by restoring write permissions.
static void makeTreeDeletable(const std::filesystem::path & path)
{
    for (auto & entry : std::filesystem::recursive_directory_iterator(
             path, std::filesystem::directory_options::skip_permission_denied)) {
        if (entry.is_directory())
            ::chmod(entry.path().c_str(), 0755);
        else
            ::chmod(entry.path().c_str(), 0644);
    }
    ::chmod(path.c_str(), 0755);
}

static std::filesystem::path createTestTree(const std::filesystem::path & parent, int fileCount)
{
    auto root = parent / "bench-tree";
    std::filesystem::create_directories(root);

    int filesPerDir = std::max(1, fileCount / 10);
    int dirIdx = 0;
    auto currentDir = root;

    for (int i = 0; i < fileCount; ++i) {
        if (i % filesPerDir == 0) {
            currentDir = root / fmt("dir%d", dirIdx++);
            std::filesystem::create_directories(currentDir);
        }
        auto filePath = currentDir / fmt("file%d.txt", i);
        std::ofstream out(filePath, std::ios::binary);
        out << fmt("content-of-file-%d-padding-to-make-it-longer-for-scanning-%d", i, i);
        out.close();
        ::chmod(filePath.c_str(), 0644);
    }

    return root;
}

/**
 * Simulates master's 3-pass output registration pipeline:
 *   Pass 1: canonicalisePathMetaData() — separate walk (lstat + chmod + lchown + utimes)
 *   Pass 2: scanForReferences(NullSink) — scan walk with hash DISCARDED
 *   Pass 3: dumpPath() into HashSink — third full tree walk recomputing the NAR hash
 */
static void BM_ThreePass_Master(benchmark::State & state)
{
    const int fileCount = state.range(0);
    StorePathSet emptyRefs;

    for (auto _ : state) {
        state.PauseTiming();
        auto tmpDir = createTempDir();
        auto root = createTestTree(tmpDir, fileCount);
        state.ResumeTiming();

        // Pass 1: Separate canonicalize walk
        InodesSeen inodesSeen;
        CanonicalizePathMetadataOptions options BENCH_CANON_OPTIONS;
        canonicalisePathMetaData(root, options, inodesSeen);

        // Pass 2: Scan for references (hash DISCARDED — master used NullSink)
        NullSink nullSink;
        scanForReferences(nullSink, root, emptyRefs);

        // Pass 3: Separate hash computation (third full tree walk)
        HashSink narHashSink{HashAlgorithm::SHA256};
        dumpPath(root, narHashSink);
        auto hash = narHashSink.finish();
        benchmark::DoNotOptimize(hash);

        state.PauseTiming();
        makeTreeDeletable(tmpDir);
        std::filesystem::remove_all(tmpDir);
        state.ResumeTiming();
    }

    state.SetItemsProcessed(state.iterations() * fileCount);
}

/**
 * After commit 1 — scan and hash merged into one pass:
 *   Pass 1: canonicalisePathMetaData() — separate walk
 *   Pass 2: scanForReferences(HashSink) — fused scan + hash
 */
static void BM_TwoPass_ScanHashMerged(benchmark::State & state)
{
    const int fileCount = state.range(0);
    StorePathSet emptyRefs;

    for (auto _ : state) {
        state.PauseTiming();
        auto tmpDir = createTempDir();
        auto root = createTestTree(tmpDir, fileCount);
        state.ResumeTiming();

        // Pass 1: Separate canonicalize walk
        InodesSeen inodesSeen;
        CanonicalizePathMetadataOptions options BENCH_CANON_OPTIONS;
        canonicalisePathMetaData(root, options, inodesSeen);

        // Pass 2: Fused scan + hash (commit 1's optimization)
        HashSink narHashSink{HashAlgorithm::SHA256};
        scanForReferences(narHashSink, root, emptyRefs);
        auto hash = narHashSink.finish();
        benchmark::DoNotOptimize(hash);

        state.PauseTiming();
        makeTreeDeletable(tmpDir);
        std::filesystem::remove_all(tmpDir);
        state.ResumeTiming();
    }

    state.SetItemsProcessed(state.iterations() * fileCount);
}

/**
 * All 3 commits — single traversal via CanonicalizingSourceAccessor:
 *   Single pass: canonicalize + scan + hash fused together
 */
static void BM_SinglePass_FullyFused(benchmark::State & state)
{
    const int fileCount = state.range(0);
    StorePathSet emptyRefs;

    for (auto _ : state) {
        state.PauseTiming();
        auto tmpDir = createTempDir();
        auto root = createTestTree(tmpDir, fileCount);
        state.ResumeTiming();

        InodesSeen inodesSeen;
        CanonicalizePathMetadataOptions options BENCH_CANON_OPTIONS;

        // Single fused pass: canonicalize + scan + hash
        auto sourcePath = PosixSourceAccessor::createAtRoot(root);
        auto canonAccessor = make_ref<CanonicalizingSourceAccessor>(sourcePath.accessor, options, inodesSeen);

        HashSink narHashSink{HashAlgorithm::SHA256};
        scanForReferences(narHashSink, SourcePath{canonAccessor, sourcePath.path}, emptyRefs);
        auto hash = narHashSink.finish();
        benchmark::DoNotOptimize(hash);

        state.PauseTiming();
        makeTreeDeletable(tmpDir);
        std::filesystem::remove_all(tmpDir);
        state.ResumeTiming();
    }

    state.SetItemsProcessed(state.iterations() * fileCount);
}

BENCHMARK(BM_ThreePass_Master)->Arg(100)->Arg(1000)->Arg(5000)->Arg(10000)->Arg(20000);
BENCHMARK(BM_TwoPass_ScanHashMerged)->Arg(100)->Arg(1000)->Arg(5000)->Arg(10000)->Arg(20000);
BENCHMARK(BM_SinglePass_FullyFused)->Arg(100)->Arg(1000)->Arg(5000)->Arg(10000)->Arg(20000);

#endif
