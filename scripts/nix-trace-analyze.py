#!/usr/bin/env python3
"""
nix-trace-analyze: Post-processing tool for Nix build telemetry traces.

Reads OTLP JSONL trace files produced by Nix's telemetry system and computes
percentile distributions across all builds, answering questions like:
  - "What is the p95 buildPhase duration across all packages?"
  - "How much time is spent in sandbox setup vs actual building?"
  - "What does the lock wait distribution look like?"

Usage:
  nix-trace-analyze [OPTIONS] [TRACE_DIR]

  TRACE_DIR defaults to /nix/var/log/nix/traces

Examples:
  # Analyze all traces with default settings
  nix-trace-analyze

  # Analyze traces in a custom directory, show top-10 slowest builds
  nix-trace-analyze --top 10 /tmp/my-traces

  # Filter to only buildPhase and installPhase spans
  nix-trace-analyze --filter 'buildPhase,installPhase'

  # Output as JSON for further processing
  nix-trace-analyze --json

  # Show per-derivation breakdown for a specific derivation
  nix-trace-analyze --drv hello
"""

import argparse
import glob
import json
import math
import os
import sys
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional


@dataclass
class SketchData:
    """Accumulates samples and computes quantiles via sorted-sample interpolation."""

    samples: list[float] = field(default_factory=list)
    count: int = 0
    total: float = 0.0
    min_val: float = float("inf")
    max_val: float = float("-inf")

    def update(self, value: float):
        self.samples.append(value)
        self.count += 1
        self.total += value
        self.min_val = min(self.min_val, value)
        self.max_val = max(self.max_val, value)

    def quantile(self, p: float) -> float:
        if not self.samples:
            return 0.0
        sorted_samples = sorted(self.samples)
        idx = p * (len(sorted_samples) - 1)
        lo = int(idx)
        hi = min(lo + 1, len(sorted_samples) - 1)
        frac = idx - lo
        return sorted_samples[lo] * (1.0 - frac) + sorted_samples[hi] * frac

    def mean(self) -> float:
        return self.total / self.count if self.count else 0.0

    def stddev(self) -> float:
        if self.count < 2:
            return 0.0
        mean = self.mean()
        variance = sum((s - mean) ** 2 for s in self.samples) / (self.count - 1)
        return math.sqrt(variance)


@dataclass
class SpanRecord:
    """Parsed span from OTLP JSONL."""

    trace_id: str
    span_id: str
    parent_span_id: str
    name: str
    start_ns: int
    end_ns: int
    duration_us: float  # microseconds
    duration_s: float  # seconds
    attributes: dict[str, str]
    drv_path: str  # extracted from attributes


def parse_trace_file(path: str) -> list[SpanRecord]:
    """Parse a single JSONL trace file into SpanRecords."""
    spans = []
    with open(path, "r") as f:
        for line_num, line in enumerate(f, 1):
            line = line.strip()
            if not line:
                continue
            try:
                obj = json.loads(line)
            except json.JSONDecodeError as e:
                print(
                    f"  warning: {path}:{line_num}: skipping malformed JSON: {e}",
                    file=sys.stderr,
                )
                continue

            start_ns = obj.get("startTimeUnixNano", 0)
            end_ns = obj.get("endTimeUnixNano", 0)
            duration_ns = end_ns - start_ns
            duration_us = duration_ns / 1000.0
            duration_s = duration_ns / 1_000_000_000.0

            # Extract attributes into flat dict
            attrs = {}
            for attr in obj.get("attributes", []):
                key = attr.get("key", "")
                value = attr.get("value", {}).get("stringValue", "")
                if key and value:
                    attrs[key] = value

            spans.append(
                SpanRecord(
                    trace_id=obj.get("traceId", ""),
                    span_id=obj.get("spanId", ""),
                    parent_span_id=obj.get("parentSpanId", ""),
                    name=obj.get("name", ""),
                    start_ns=start_ns,
                    end_ns=end_ns,
                    duration_us=duration_us,
                    duration_s=duration_s,
                    attributes=attrs,
                    drv_path=attrs.get("drv.path", ""),
                )
            )
    return spans


def load_all_traces(trace_dir: str) -> list[SpanRecord]:
    """Load spans from all trace files (current + rotated)."""
    all_spans = []

    # Current file
    current = os.path.join(trace_dir, "nix-traces.jsonl")
    if os.path.exists(current):
        all_spans.extend(parse_trace_file(current))

    # Rotated files (.1, .2, ...)
    for rotated in sorted(glob.glob(os.path.join(trace_dir, "nix-traces.jsonl.*"))):
        # Only numeric suffixes
        suffix = rotated.rsplit(".", 1)[-1]
        if suffix.isdigit():
            all_spans.extend(parse_trace_file(rotated))

    return all_spans


# Known phase names (stdenv phases) vs pipeline stage names
PIPELINE_STAGES = {
    "inputSubstitution",
    "lockWait",
    "sandboxSetup",
    "builderExecution",
    "outputRegistration",
    "postBuildHook",
}

PHASE_NAMES = {
    "unpackPhase",
    "patchPhase",
    "configurePhase",
    "buildPhase",
    "checkPhase",
    "installPhase",
    "fixupPhase",
    "distributePhase",
}


def classify_span(name: str) -> str:
    """Classify a span as 'phase', 'pipeline', or 'other'."""
    if name in PIPELINE_STAGES:
        return "pipeline"
    if name in PHASE_NAMES or name.endswith("Phase"):
        return "phase"
    return "other"


def format_duration(seconds: float) -> str:
    """Format a duration in seconds to a human-readable string."""
    if seconds < 0.001:
        return f"{seconds * 1_000_000:.0f}us"
    if seconds < 1.0:
        return f"{seconds * 1000:.1f}ms"
    if seconds < 60.0:
        return f"{seconds:.2f}s"
    if seconds < 3600.0:
        minutes = int(seconds // 60)
        secs = seconds % 60
        return f"{minutes}m{secs:.1f}s"
    hours = int(seconds // 3600)
    minutes = int((seconds % 3600) // 60)
    return f"{hours}h{minutes}m"


def print_distribution_table(
    title: str, sketches: dict[str, SketchData], sort_by: str = "p50"
):
    """Print a formatted table of quantile distributions."""
    if not sketches:
        return

    print(f"\n{'=' * 80}")
    print(f"  {title}")
    print(f"{'=' * 80}")

    # Header
    print(
        f"  {'Name':<25} {'Count':>6} {'p50':>10} {'p90':>10} {'p95':>10} {'p99':>10} {'Max':>10}"
    )
    print(f"  {'-' * 25} {'-' * 6} {'-' * 10} {'-' * 10} {'-' * 10} {'-' * 10} {'-' * 10}")

    # Sort by median descending
    items = []
    for name, sketch in sketches.items():
        if sketch.count == 0:
            continue
        items.append((name, sketch))

    items.sort(key=lambda x: x[1].quantile(0.50), reverse=True)

    for name, sketch in items:
        p50 = format_duration(sketch.quantile(0.50))
        p90 = format_duration(sketch.quantile(0.90))
        p95 = format_duration(sketch.quantile(0.95))
        p99 = format_duration(sketch.quantile(0.99))
        mx = format_duration(sketch.max_val)
        print(f"  {name:<25} {sketch.count:>6} {p50:>10} {p90:>10} {p95:>10} {p99:>10} {mx:>10}")


def print_top_slowest(title: str, spans: list[SpanRecord], n: int = 10):
    """Print the N slowest spans."""
    if not spans:
        return

    print(f"\n{'=' * 80}")
    print(f"  {title} (top {n})")
    print(f"{'=' * 80}")

    sorted_spans = sorted(spans, key=lambda s: s.duration_s, reverse=True)[:n]

    for i, span in enumerate(sorted_spans, 1):
        drv = span.drv_path
        # Shorten derivation path for display
        if drv:
            drv_short = drv.split("/")[-1] if "/" in drv else drv
            if len(drv_short) > 50:
                drv_short = drv_short[:47] + "..."
        else:
            drv_short = "(unknown)"
        print(f"  {i:>3}. {format_duration(span.duration_s):>10}  {span.name:<20} {drv_short}")


def print_pipeline_breakdown(pipeline_sketches: dict[str, SketchData]):
    """Print pipeline stage breakdown showing where time goes."""
    if not pipeline_sketches:
        return

    total_time = sum(s.total for s in pipeline_sketches.values())
    if total_time == 0:
        return

    print(f"\n{'=' * 80}")
    print(f"  Pipeline Time Breakdown (total across all builds)")
    print(f"{'=' * 80}")

    items = sorted(
        pipeline_sketches.items(), key=lambda x: x[1].total, reverse=True
    )

    for name, sketch in items:
        pct = (sketch.total / total_time) * 100
        bar_len = int(pct / 2)
        bar = "#" * bar_len
        print(
            f"  {name:<25} {format_duration(sketch.total):>10} ({pct:5.1f}%)  {bar}"
        )

    print(f"  {'TOTAL':<25} {format_duration(total_time):>10}")


def analyze_per_derivation(
    spans: list[SpanRecord], drv_filter: Optional[str] = None
) -> dict[str, list[SpanRecord]]:
    """Group spans by derivation path."""
    by_drv: dict[str, list[SpanRecord]] = defaultdict(list)
    for span in spans:
        if not span.drv_path:
            continue
        if drv_filter and drv_filter not in span.drv_path:
            continue
        by_drv[span.drv_path].append(span)
    return dict(by_drv)


def output_json(
    phase_sketches: dict[str, SketchData],
    pipeline_sketches: dict[str, SketchData],
    spans: list[SpanRecord],
):
    """Output analysis results as JSON."""
    result = {
        "phases": {},
        "pipeline": {},
        "summary": {
            "totalSpans": len(spans),
            "uniqueDerivations": len(set(s.drv_path for s in spans if s.drv_path)),
        },
    }

    for category, sketches in [
        ("phases", phase_sketches),
        ("pipeline", pipeline_sketches),
    ]:
        for name, sketch in sketches.items():
            if sketch.count == 0:
                continue
            result[category][name] = {
                "count": sketch.count,
                "mean": sketch.mean(),
                "stddev": sketch.stddev(),
                "min": sketch.min_val,
                "max": sketch.max_val,
                "p50": sketch.quantile(0.50),
                "p90": sketch.quantile(0.90),
                "p95": sketch.quantile(0.95),
                "p99": sketch.quantile(0.99),
                "totalSeconds": sketch.total,
            }

    json.dump(result, sys.stdout, indent=2)
    print()


def main():
    parser = argparse.ArgumentParser(
        description="Analyze Nix build telemetry traces",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument(
        "trace_dir",
        nargs="?",
        default="/nix/var/log/nix/traces",
        help="Directory containing nix-traces.jsonl files (default: /nix/var/log/nix/traces)",
    )
    parser.add_argument(
        "--top",
        type=int,
        default=10,
        help="Show top N slowest builds/phases (default: 10)",
    )
    parser.add_argument(
        "--filter",
        type=str,
        default=None,
        help="Comma-separated list of span names to include",
    )
    parser.add_argument(
        "--drv",
        type=str,
        default=None,
        help="Filter to derivations matching this substring",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="Output results as JSON",
    )
    parser.add_argument(
        "--prometheus",
        action="store_true",
        help="Output results in Prometheus text exposition format",
    )

    args = parser.parse_args()

    # Load traces
    trace_dir = args.trace_dir
    if not os.path.isdir(trace_dir):
        print(f"error: trace directory does not exist: {trace_dir}", file=sys.stderr)
        print(
            f"hint: set telemetry-enable = true in nix.conf and run some builds first",
            file=sys.stderr,
        )
        sys.exit(1)

    spans = load_all_traces(trace_dir)
    if not spans:
        print(f"No trace data found in {trace_dir}", file=sys.stderr)
        sys.exit(1)

    # Apply filters
    name_filter = None
    if args.filter:
        name_filter = set(args.filter.split(","))

    if args.drv:
        spans = [s for s in spans if args.drv in s.drv_path]

    if name_filter:
        spans = [s for s in spans if s.name in name_filter]

    # Build sketches by category
    phase_sketches: dict[str, SketchData] = defaultdict(SketchData)
    pipeline_sketches: dict[str, SketchData] = defaultdict(SketchData)
    all_sketches: dict[str, SketchData] = defaultdict(SketchData)

    for span in spans:
        category = classify_span(span.name)
        all_sketches[span.name].update(span.duration_s)
        if category == "phase":
            phase_sketches[span.name].update(span.duration_s)
        elif category == "pipeline":
            pipeline_sketches[span.name].update(span.duration_s)

    # Output
    if args.json:
        output_json(dict(phase_sketches), dict(pipeline_sketches), spans)
        return

    if args.prometheus:
        for category_name, metric_name, label_name, sketches in [
            (
                "phases",
                "nix_build_phase_duration_seconds",
                "phase",
                phase_sketches,
            ),
            (
                "pipeline",
                "nix_build_pipeline_stage_duration_seconds",
                "stage",
                pipeline_sketches,
            ),
        ]:
            if not sketches:
                continue
            print(f"# HELP {metric_name} Duration distribution from trace analysis")
            print(f"# TYPE {metric_name} summary")
            for name, sketch in sketches.items():
                if sketch.count == 0:
                    continue
                label = f'{label_name}="{name}"'
                for q, val in [
                    ("0.5", sketch.quantile(0.50)),
                    ("0.9", sketch.quantile(0.90)),
                    ("0.95", sketch.quantile(0.95)),
                    ("0.99", sketch.quantile(0.99)),
                ]:
                    print(f'{metric_name}{{{label},quantile="{q}"}} {val}')
                print(f"{metric_name}_count{{{label}}} {sketch.count}")
        return

    # Human-readable output
    unique_drvs = len(set(s.drv_path for s in spans if s.drv_path))
    print(f"\nNix Build Telemetry Analysis")
    print(f"  Trace directory: {trace_dir}")
    print(f"  Total spans:     {len(spans)}")
    print(f"  Unique drvs:     {unique_drvs}")

    # Phase distributions
    print_distribution_table(
        "Build Phase Durations", dict(phase_sketches)
    )

    # Pipeline distributions
    print_distribution_table(
        "Pipeline Stage Durations", dict(pipeline_sketches)
    )

    # Pipeline breakdown
    print_pipeline_breakdown(dict(pipeline_sketches))

    # Top slowest
    phase_spans = [s for s in spans if classify_span(s.name) == "phase"]
    print_top_slowest("Slowest Build Phases", phase_spans, args.top)

    pipeline_spans = [s for s in spans if classify_span(s.name) == "pipeline"]
    print_top_slowest("Slowest Pipeline Stages", pipeline_spans, args.top)


if __name__ == "__main__":
    main()
