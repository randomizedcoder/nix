#pragma once
///@file

#include <chrono>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace nix::metrics {

/**
 * Histogram for accumulating duration measurements within a single process.
 *
 * Each update() inserts a single sample into the sketch for the given
 * label value. Thread-safe via internal mutex.
 *
 * These histograms are process-local: in a forking daemon, each child
 * process has its own copy that is lost on exit. For cross-process
 * aggregate analysis, use the OTLP trace files (nix-traces.jsonl) with
 * the scripts/nix-trace-analyze.py post-processing tool.
 */
struct DurationHistogram
{
    struct Quantiles
    {
        double p50 = 0, p90 = 0, p95 = 0, p99 = 0;
        double min = 0, max = 0;
        uint64_t count = 0;
    };

    /**
     * Record a duration measurement for the given label.
     */
    void update(const std::string & label, std::chrono::microseconds duration);

    /**
     * Record a raw value for the given label (no unit conversion).
     * Use for non-duration metrics like memory bytes.
     */
    void updateRaw(const std::string & label, double value);

    /**
     * Query quantiles for a given label.
     */
    Quantiles query(const std::string & label) const;

    /**
     * Get all labels that have been recorded.
     */
    std::vector<std::string> labels() const;

    /**
     * Merge another histogram's data into this one (for child->parent merge).
     */
    void merge(const DurationHistogram & other);

    /**
     * Serialize the sketch state for transport (e.g., over socketpair).
     */
    std::string serialize() const;

    /**
     * Deserialize and merge sketch state.
     */
    void deserializeAndMerge(const std::string & data);

private:
    mutable std::mutex mutex_;

    struct SketchData
    {
        std::vector<double> samples;
        uint64_t count = 0;
        double min = std::numeric_limits<double>::max();
        double max = std::numeric_limits<double>::lowest();
    };

    std::map<std::string, SketchData> sketches_;
};

/**
 * Process-global histograms for build telemetry.
 *
 * These accumulate measurements across all concurrent builds in this
 * process. In a forking daemon, each child has its own copy -- for
 * aggregate distributions across all builds, analyze the OTLP trace
 * files with scripts/nix-trace-analyze.py.
 */

/** Per-phase duration distribution (e.g., buildPhase, installPhase) */
extern DurationHistogram buildPhaseSeconds;

/** Per-pipeline-stage duration distribution */
extern DurationHistogram pipelineStageSeconds;

/** Memory usage distribution per phase */
extern DurationHistogram buildMemoryBytes;

/**
 * OTLP span representation for build telemetry.
 */
struct OtlpSpan
{
    std::string traceId;
    std::string spanId;
    std::string parentSpanId;
    std::string name;
    std::chrono::system_clock::time_point startTime;
    std::chrono::system_clock::time_point endTime;
    std::map<std::string, std::string> attributes;
};

/**
 * OTLP span collector. Accumulates spans and writes them as OTLP JSON
 * (newline-delimited) to the configured trace directory.
 */
struct OtlpTraceWriter
{
    /**
     * Initialize the trace writer with the given output directory.
     * Creates the directory if it doesn't exist.
     */
    explicit OtlpTraceWriter(
        const std::string & traceDir,
        uint64_t maxFileSize = 100 * 1024 * 1024,
        unsigned int maxFiles = 10);

    /**
     * Write a span to the trace file.
     */
    void writeSpan(const OtlpSpan & span);

    /**
     * Perform log rotation if the current file exceeds maxFileSize.
     */
    void maybeRotate();

private:
    std::string traceDir_;
    uint64_t maxFileSize_;
    unsigned int maxFiles_;
    mutable std::mutex mutex_;
};

/**
 * Generate a deterministic trace ID from a derivation path.
 * Produces the same trace ID for retries/rebuilds of the same derivation.
 */
std::string traceIdFromDrvPath(const std::string & drvPath);

/**
 * Generate a random span ID.
 */
std::string randomSpanId();

/**
 * Render all process-global histograms as Prometheus text exposition format.
 *
 * Output includes summary-style metrics with quantile labels for each
 * histogram/label combination:
 *
 *   nix_build_phase_duration_seconds{phase="buildPhase",quantile="0.5"} 12.34
 *   nix_build_phase_duration_seconds{phase="buildPhase",quantile="0.9"} 45.67
 *   ...
 *   nix_build_phase_duration_seconds_count{phase="buildPhase"} 100
 */
std::string renderPrometheusMetrics();

} // namespace nix::metrics
