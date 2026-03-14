#include "nix/metrics/build-metrics.hh"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>

namespace nix::metrics {

/* Process-global histogram instances */
DurationHistogram buildPhaseSeconds;
DurationHistogram pipelineStageSeconds;
DurationHistogram buildMemoryBytes;

void DurationHistogram::update(const std::string & label, std::chrono::microseconds duration)
{
    double seconds = static_cast<double>(duration.count()) / 1000000.0;
    updateRaw(label, seconds);
}

void DurationHistogram::updateRaw(const std::string & label, double value)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto & sketch = sketches_[label];
    sketch.samples.push_back(value);
    sketch.count++;
    sketch.min = std::min(sketch.min, value);
    sketch.max = std::max(sketch.max, value);
}

DurationHistogram::Quantiles DurationHistogram::query(const std::string & label) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sketches_.find(label);
    if (it == sketches_.end())
        return {};

    auto & sketch = it->second;
    if (sketch.samples.empty())
        return {};

    /* Sort a copy for quantile computation */
    auto sorted = sketch.samples;
    std::sort(sorted.begin(), sorted.end());

    auto quantile = [&](double p) -> double {
        if (sorted.empty())
            return 0;
        double idx = p * (sorted.size() - 1);
        size_t lo = static_cast<size_t>(idx);
        size_t hi = std::min(lo + 1, sorted.size() - 1);
        double frac = idx - lo;
        return sorted[lo] * (1.0 - frac) + sorted[hi] * frac;
    };

    return {
        .p50 = quantile(0.50),
        .p90 = quantile(0.90),
        .p95 = quantile(0.95),
        .p99 = quantile(0.99),
        .min = sketch.min,
        .max = sketch.max,
        .count = sketch.count,
    };
}

std::vector<std::string> DurationHistogram::labels() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> result;
    result.reserve(sketches_.size());
    for (auto & [label, _] : sketches_)
        result.push_back(label);
    return result;
}

void DurationHistogram::merge(const DurationHistogram & other)
{
    std::scoped_lock lock(mutex_, other.mutex_);
    for (auto & [label, otherSketch] : other.sketches_) {
        auto & sketch = sketches_[label];
        sketch.samples.insert(sketch.samples.end(), otherSketch.samples.begin(), otherSketch.samples.end());
        sketch.count += otherSketch.count;
        sketch.min = std::min(sketch.min, otherSketch.min);
        sketch.max = std::max(sketch.max, otherSketch.max);
    }
}

std::string DurationHistogram::serialize() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::ostringstream oss;

    uint64_t numLabels = sketches_.size();
    oss.write(reinterpret_cast<const char *>(&numLabels), sizeof(numLabels));

    for (auto & [label, sketch] : sketches_) {
        uint64_t labelLen = label.size();
        oss.write(reinterpret_cast<const char *>(&labelLen), sizeof(labelLen));
        oss.write(label.data(), labelLen);

        oss.write(reinterpret_cast<const char *>(&sketch.count), sizeof(sketch.count));
        oss.write(reinterpret_cast<const char *>(&sketch.min), sizeof(sketch.min));
        oss.write(reinterpret_cast<const char *>(&sketch.max), sizeof(sketch.max));

        uint64_t numSamples = sketch.samples.size();
        oss.write(reinterpret_cast<const char *>(&numSamples), sizeof(numSamples));
        for (auto & s : sketch.samples)
            oss.write(reinterpret_cast<const char *>(&s), sizeof(s));
    }

    return oss.str();
}

void DurationHistogram::deserializeAndMerge(const std::string & data)
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::istringstream iss(data);

    uint64_t numLabels;
    iss.read(reinterpret_cast<char *>(&numLabels), sizeof(numLabels));

    for (uint64_t i = 0; i < numLabels; ++i) {
        uint64_t labelLen;
        iss.read(reinterpret_cast<char *>(&labelLen), sizeof(labelLen));
        std::string label(labelLen, '\0');
        iss.read(label.data(), labelLen);

        SketchData incoming;
        iss.read(reinterpret_cast<char *>(&incoming.count), sizeof(incoming.count));
        iss.read(reinterpret_cast<char *>(&incoming.min), sizeof(incoming.min));
        iss.read(reinterpret_cast<char *>(&incoming.max), sizeof(incoming.max));

        uint64_t numSamples;
        iss.read(reinterpret_cast<char *>(&numSamples), sizeof(numSamples));
        incoming.samples.resize(numSamples);
        for (uint64_t j = 0; j < numSamples; ++j)
            iss.read(reinterpret_cast<char *>(&incoming.samples[j]), sizeof(double));

        auto & sketch = sketches_[label];
        sketch.samples.insert(sketch.samples.end(), incoming.samples.begin(), incoming.samples.end());
        sketch.count += incoming.count;
        sketch.min = std::min(sketch.min, incoming.min);
        sketch.max = std::max(sketch.max, incoming.max);
    }
}

OtlpTraceWriter::OtlpTraceWriter(const std::string & traceDir, uint64_t maxFileSize, unsigned int maxFiles)
    : traceDir_(traceDir)
    , maxFileSize_(maxFileSize)
    , maxFiles_(maxFiles)
{
}

void OtlpTraceWriter::writeSpan(const OtlpSpan & span)
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto startNano =
        std::chrono::duration_cast<std::chrono::nanoseconds>(span.startTime.time_since_epoch()).count();
    auto endNano =
        std::chrono::duration_cast<std::chrono::nanoseconds>(span.endTime.time_since_epoch()).count();

    /* Write OTLP-compatible JSON line */
    std::ostringstream oss;
    oss << "{\"traceId\":\"" << span.traceId << "\""
        << ",\"spanId\":\"" << span.spanId << "\"";
    if (!span.parentSpanId.empty())
        oss << ",\"parentSpanId\":\"" << span.parentSpanId << "\"";
    oss << ",\"name\":\"" << span.name << "\""
        << ",\"startTimeUnixNano\":" << startNano
        << ",\"endTimeUnixNano\":" << endNano;

    if (!span.attributes.empty()) {
        oss << ",\"attributes\":[";
        bool first = true;
        for (auto & [k, v] : span.attributes) {
            if (!first)
                oss << ",";
            oss << "{\"key\":\"" << k << "\",\"value\":{\"stringValue\":\"" << v << "\"}}";
            first = false;
        }
        oss << "]";
    }

    oss << "}\n";

    auto tracePath = traceDir_ + "/nix-traces.jsonl";
    std::ofstream out(tracePath, std::ios::app);
    if (out.is_open())
        out << oss.str();
}

void OtlpTraceWriter::maybeRotate()
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto tracePath = traceDir_ + "/nix-traces.jsonl";

    /* Check current file size */
    std::ifstream in(tracePath, std::ios::ate | std::ios::binary);
    if (!in.is_open())
        return;

    auto size = static_cast<uint64_t>(in.tellg());
    in.close();

    if (size < maxFileSize_)
        return;

    /* Rotate: shift existing files */
    for (unsigned int i = maxFiles_ - 1; i > 0; --i) {
        auto src = tracePath + "." + std::to_string(i);
        auto dst = tracePath + "." + std::to_string(i + 1);
        std::rename(src.c_str(), dst.c_str());
    }

    /* Delete oldest if it exceeds maxFiles */
    auto oldest = tracePath + "." + std::to_string(maxFiles_);
    std::remove(oldest.c_str());

    /* Rotate current → .1 */
    std::rename(tracePath.c_str(), (tracePath + ".1").c_str());
}

std::string traceIdFromDrvPath(const std::string & drvPath)
{
    /* Simple hash-based trace ID: SHA-256 truncated to 16 bytes (32 hex chars) */
    /* For now, use a simple FNV-1a hash to produce a deterministic 128-bit ID */
    uint64_t h1 = 14695981039346656037ULL;
    uint64_t h2 = 14695981039346656037ULL;
    for (size_t i = 0; i < drvPath.size(); ++i) {
        auto & h = (i % 2 == 0) ? h1 : h2;
        h ^= static_cast<uint64_t>(drvPath[i]);
        h *= 1099511628211ULL;
    }

    std::ostringstream oss;
    oss << std::hex << std::setfill('0') << std::setw(16) << h1 << std::setw(16) << h2;
    return oss.str();
}

std::string randomSpanId()
{
    static thread_local std::mt19937_64 rng(std::random_device{}());
    std::uniform_int_distribution<uint64_t> dist;
    uint64_t val = dist(rng);

    std::ostringstream oss;
    oss << std::hex << std::setfill('0') << std::setw(16) << val;
    return oss.str();
}

namespace {

/**
 * Render a single histogram as Prometheus text exposition format.
 */
void renderHistogram(
    std::ostringstream & oss,
    const std::string & metricName,
    const std::string & labelName,
    DurationHistogram & histogram)
{
    auto allLabels = histogram.labels();
    if (allLabels.empty())
        return;

    oss << "# HELP " << metricName << " Duration histogram from KLL sketch\n";
    oss << "# TYPE " << metricName << " summary\n";

    for (auto & label : allLabels) {
        auto q = histogram.query(label);
        if (q.count == 0)
            continue;

        auto labelStr = labelName + "=\"" + label + "\"";

        oss << metricName << "{" << labelStr << ",quantile=\"0.5\"} " << q.p50 << "\n";
        oss << metricName << "{" << labelStr << ",quantile=\"0.9\"} " << q.p90 << "\n";
        oss << metricName << "{" << labelStr << ",quantile=\"0.95\"} " << q.p95 << "\n";
        oss << metricName << "{" << labelStr << ",quantile=\"0.99\"} " << q.p99 << "\n";
        oss << metricName << "_min{" << labelStr << "} " << q.min << "\n";
        oss << metricName << "_max{" << labelStr << "} " << q.max << "\n";
        oss << metricName << "_count{" << labelStr << "} " << q.count << "\n";
    }
}

} // anonymous namespace

std::string renderPrometheusMetrics()
{
    std::ostringstream oss;
    renderHistogram(oss, "nix_build_phase_duration_seconds", "phase", buildPhaseSeconds);
    renderHistogram(oss, "nix_build_pipeline_stage_duration_seconds", "stage", pipelineStageSeconds);
    renderHistogram(oss, "nix_build_memory_bytes", "phase", buildMemoryBytes);
    return oss.str();
}

} // namespace nix::metrics
