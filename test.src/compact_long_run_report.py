#!/usr/bin/env python3
"""Summarize long Prover9 periodic reports as CPU/RAM slope evidence.

The parser accepts current and historical plain or gzip-compressed .out files.
It deliberately derives interval rates from cumulative counters: a final total
can hide exactly the late-run collapse this tool is intended to expose.
"""

import argparse
import gzip
import json
import math
import os
import re
import sys


GIVEN_RE = re.compile(
    r"^Given=(\d+)\. Generated=(\d+)\. Kept=(\d+)\. proofs=(\d+)\.")
STATE_RE = re.compile(
    r"^Usable=(\d+)\. Sos=(\d+)\. Demods=(\d+)\. Limbo=(\d+), "
    r"Disabled=(\d+)\. Hints=(\d+)\. Active_Hints=(\d+)\.")
CLOCK_RE = re.compile(
    r"^clock\s+([A-Za-z0-9_]+)\s*:\s*([0-9]+(?:\.[0-9]+)?) seconds\.")
NUMBER_RE = re.compile(r"^([-+]?[0-9]+(?:\.[0-9]+)?)")
IO_RE = re.compile(r"^([0-9]+) \(([0-9]+) bytes\)$")

PREFIXES = (
    ("Compact_back_demod:", "back"),
    ("Dense_passive:", "passive"),
    ("Dense_passive_selector:", "selector"),
    ("Dense_passive_gc:", "passive_gc"),
    ("Ancestor_store:", "ancestor"),
    ("Process_residency_kb:", "residency"),
    ("Compact_otter_demodulation:", "rewrite"),
)

STATISTICS_FORMAT_MARKER = "Statistics_format:"
SAFE_COMMA_NUM_BUFFERS = 24

CUMULATIVE_KEYS = (
    "given", "generated", "kept", "user_cpu", "system_cpu",
    "back_queries", "back_groups_examined", "back_tree_nodes_examined",
    "back_tree_sibling_checks", "back_tree_child_lookups",
    "back_candidates", "back_timing_lookup_seconds",
    "back_timing_exact_seconds", "back_timing_materialize_seconds",
    "back_timing_maintenance_seconds", "demod_attempts", "demod_rewrites",
    "clock_infer", "clock_preprocess", "clock_demod", "clock_hints",
    "clock_subsume", "clock_back_demod", "ancestor_file_reads_bytes",
    "ancestor_file_writes_bytes", "selector_reads_bytes",
    "selector_writes_bytes",
)


def scalar(text):
    """Return an int/float when the complete leading scalar is numeric."""
    text = text.strip().rstrip(".")
    match = NUMBER_RE.match(text)
    if match and match.end() == len(text):
        number = match.group(1)
        return float(number) if "." in number else int(number)
    return text


def assignments(payload):
    """Parse comma-separated key=value reports and expose '(N bytes)' too."""
    result = {}
    for item in payload.strip().rstrip(".").split(", "):
        if "=" not in item:
            continue
        key, value = item.split("=", 1)
        io_match = IO_RE.match(value)
        if io_match:
            result[key] = int(io_match.group(1))
            result[key + "_bytes"] = int(io_match.group(2))
        else:
            result[key] = scalar(value)
    return result


def add_prefixed(sample, prefix, values):
    for key, value in values.items():
        sample[prefix + "_" + key] = value


def open_output(path):
    if path == "-":
        return sys.stdin
    if path.endswith(".gz"):
        return gzip.open(path, "rt", encoding="utf-8", errors="replace")
    return open(path, "r", encoding="utf-8", errors="replace")


def parse_stream(lines, source="<stream>"):
    samples = []
    sample = None
    statistics_format = {}
    for line_number, raw in enumerate(lines, 1):
        line = raw.rstrip("\n")
        if line.startswith(STATISTICS_FORMAT_MARKER):
            statistics_format = {}
            add_prefixed(
                statistics_format, "statistics_format",
                assignments(line[len(STATISTICS_FORMAT_MARKER):]))
            continue
        match = GIVEN_RE.match(line)
        if match:
            if sample is not None:
                samples.append(sample)
            sample = {
                "source": source,
                "source_line": line_number,
                "given": int(match.group(1)),
                "generated": int(match.group(2)),
                "kept": int(match.group(3)),
                "proofs": int(match.group(4)),
            }
            sample.update(statistics_format)
            continue
        if sample is None:
            continue
        match = STATE_RE.match(line)
        if match:
            for key, value in zip(
                    ("usable", "sos", "demods", "limbo", "disabled",
                     "hints", "active_hints"), match.groups()):
                sample[key] = int(value)
            continue
        match = CLOCK_RE.match(line)
        if match:
            sample["clock_" + match.group(1)] = float(match.group(2))
            continue
        if line.startswith("User_CPU="):
            values = assignments(line)
            sample["user_cpu"] = values.get("User_CPU")
            sample["system_cpu"] = values.get("System_CPU")
            sample["wall_clock"] = values.get("Wall_clock")
            continue
        if line.startswith("Demod_attempts="):
            values = assignments(line)
            sample["demod_attempts"] = values.get("Demod_attempts")
            sample["demod_rewrites"] = values.get("Demod_rewrites")
            continue
        if line.startswith("Compact_index_timing:"):
            values = assignments(line.split(":", 1)[1])
            component = values.get("component")
            if component == "back_demod":
                add_prefixed(sample, "back_timing", values)
            continue
        if line.startswith("Compact_query_profile:"):
            values = assignments(line.split(":", 1)[1])
            if (values.get("component") == "back_demod" and
                    values.get("op") == "candidate_lookup"):
                add_prefixed(sample, "back_profile", values)
            continue
        for marker, prefix in PREFIXES:
            if line.startswith(marker):
                add_prefixed(sample, prefix,
                             assignments(line[len(marker):]))
                break
    if sample is not None:
        samples.append(sample)
    return samples


def parse_file(path):
    stream = open_output(path)
    try:
        return parse_stream(stream, path)
    finally:
        if stream is not sys.stdin:
            stream.close()


def number(sample, key, default=0):
    value = sample.get(key, default)
    return value if isinstance(value, (int, float)) else default


def safe_ratio(numerator, denominator):
    if (not isinstance(numerator, (int, float)) or
            not isinstance(denominator, (int, float)) or denominator <= 0):
        return None
    return float(numerator) / float(denominator)


def derive_intervals(samples):
    previous = {key: 0 for key in CUMULATIVE_KEYS}
    derived = []
    for ordinal, sample in enumerate(samples, 1):
        row = dict(sample)
        row["sample"] = ordinal
        row["counter_reset"] = False
        for key in CUMULATIVE_KEYS:
            current = number(sample, key, None)
            prior = number(previous, key, None)
            if current is None or prior is None:
                delta = None
            elif current < prior:
                # Concatenated checkpoint/restart logs can reset cumulative
                # counters.  Treat the current block as the first interval of
                # a new segment instead of emitting a meaningless negative
                # rate, and make the discontinuity visible in the report.
                delta = current
                row["counter_reset"] = True
            else:
                delta = current - prior
            row["delta_" + key] = delta
        delta_cpu = row.get("delta_user_cpu")
        delta_given = row.get("delta_given")
        delta_queries = row.get("delta_back_queries")
        delta_groups = row.get("delta_back_groups_examined")
        delta_nodes = row.get("delta_back_tree_nodes_examined")
        delta_siblings = row.get("delta_back_tree_sibling_checks")
        delta_children = row.get("delta_back_tree_child_lookups")
        components = (delta_groups, delta_nodes, delta_siblings,
                      delta_children)
        combined = (sum(value for value in components if value is not None)
                    if any(value is not None for value in components) else None)
        row["delta_back_combined_work"] = combined
        row["given_per_cpu"] = safe_ratio(delta_given, delta_cpu)
        row["generated_per_given"] = safe_ratio(
            row.get("delta_generated"), delta_given)
        row["kept_per_given"] = safe_ratio(row.get("delta_kept"), delta_given)
        row["back_groups_per_query"] = safe_ratio(delta_groups, delta_queries)
        row["back_nodes_per_query"] = safe_ratio(delta_nodes, delta_queries)
        row["back_siblings_per_query"] = safe_ratio(
            delta_siblings, delta_queries)
        row["back_children_per_query"] = safe_ratio(
            delta_children, delta_queries)
        row["back_combined_per_query"] = safe_ratio(combined, delta_queries)
        row["back_lookup_cpu_pct"] = (None if delta_cpu is None else
            safe_ratio(row.get("delta_back_timing_lookup_seconds"), delta_cpu))
        if row["back_lookup_cpu_pct"] is not None:
            row["back_lookup_cpu_pct"] *= 100.0
        child_hits = number(sample, "back_tree_child_hits", None)
        child_lookups = number(sample, "back_tree_child_lookups", None)
        row["back_child_hit_pct"] = safe_ratio(child_hits, child_lookups)
        if row["back_child_hit_pct"] is not None:
            row["back_child_hit_pct"] *= 100.0
        for clock in ("infer", "preprocess", "demod", "hints", "subsume",
                      "back_demod"):
            key = "delta_clock_" + clock
            value = row.get(key)
            row["clock_" + clock + "_cpu_pct"] = (
                None if delta_cpu is None else safe_ratio(value, delta_cpu))
            if row["clock_" + clock + "_cpu_pct"] is not None:
                row["clock_" + clock + "_cpu_pct"] *= 100.0
        row["pss_mib"] = safe_ratio(number(sample, "residency_pss", None), 1024)
        row["anonymous_mib"] = safe_ratio(
            number(sample, "residency_anonymous", None), 1024)
        row["swap_mib"] = safe_ratio(number(sample, "residency_swap", None), 1024)
        row["back_mib"] = safe_ratio(number(sample, "back_bytes", None),
                                      1024 * 1024)
        ancestor_io = sum(number(row, "delta_" + key, 0) for key in
                          ("ancestor_file_reads_bytes",
                           "ancestor_file_writes_bytes"))
        selector_io = sum(number(row, "delta_" + key, 0) for key in
                          ("selector_reads_bytes", "selector_writes_bytes"))
        row["ancestor_io_mib_per_cpu"] = safe_ratio(
            ancestor_io, delta_cpu * 1024 * 1024
            if isinstance(delta_cpu, (int, float)) else None)
        row["selector_io_mib_per_cpu"] = safe_ratio(
            selector_io, delta_cpu * 1024 * 1024
            if isinstance(delta_cpu, (int, float)) else None)
        derived.append(row)
        previous = sample
    return derived


def finite(values):
    return [value for value in values
            if isinstance(value, (int, float)) and math.isfinite(value)]


def run_summary(label, rows):
    if not rows:
        return {"label": label, "samples": 0,
                "signals": ["no periodic Given= statistics blocks found"]}
    first, last = rows[0], rows[-1]
    combined = finite([row.get("back_combined_per_query") for row in rows])
    given_rates = finite([row.get("given_per_cpu") for row in rows])
    signals = []
    if len(rows) < 2:
        signals.append("only one periodic sample; interval slope is unproven")
    if number(last, "back_failures", 0) != 0:
        signals.append("compact backward-demodulation reports failures")
    if number(last, "passive_gc_validation_failures", 0) != 0:
        signals.append("dense-passive validation failures are nonzero")
    if number(last, "ancestor_validation_failures", 0) != 0:
        signals.append("ancestor-store validation failures are nonzero")
    if number(last, "residency_swap", 0) != 0:
        signals.append("process reports nonzero swap residency")
    formatting_buffers = number(
        last, "statistics_format_comma_num_buffers", 0)
    if formatting_buffers < SAFE_COMMA_NUM_BUFFERS:
        signals.append(
            "output predates the safe long-statistics formatting marker; "
            "long comma-formatted lines may contain overwritten fields")
    if any(row.get("counter_reset") for row in rows):
        signals.append("one or more cumulative counters reset between reports")
    if len(combined) >= 2 and combined[0] > 0 and combined[-1] > combined[0] * 1.25:
        signals.append("back-demod counted work/query grew by more than 25%")
    if len(given_rates) >= 2 and given_rates[0] > 0 and given_rates[-1] < given_rates[0] * 0.75:
        signals.append("given/user-CPU rate fell by more than 25%")
    return {
        "label": label,
        "samples": len(rows),
        "first_user_cpu": first.get("user_cpu"),
        "last_user_cpu": last.get("user_cpu"),
        "first_given": first.get("given"),
        "last_given": last.get("given"),
        "last_generated": last.get("generated"),
        "last_kept": last.get("kept"),
        "strategy": last.get("back_strategy"),
        "first_back_active": first.get("back_active"),
        "last_back_active": last.get("back_active"),
        "first_combined_per_query": combined[0] if combined else None,
        "last_combined_per_query": combined[-1] if combined else None,
        "combined_slope_ratio": (safe_ratio(combined[-1], combined[0])
                                 if combined else None),
        "first_given_per_cpu": given_rates[0] if given_rates else None,
        "last_given_per_cpu": given_rates[-1] if given_rates else None,
        "given_rate_ratio": (safe_ratio(given_rates[-1], given_rates[0])
                             if given_rates else None),
        "last_back_lookup_cpu_pct": last.get("back_lookup_cpu_pct"),
        "last_pss_mib": last.get("pss_mib"),
        "last_anonymous_mib": last.get("anonymous_mib"),
        "last_swap_mib": last.get("swap_mib"),
        "last_back_mib": last.get("back_mib"),
        "last_child_hit_pct": last.get("back_child_hit_pct"),
        "statistics_format_buffers": formatting_buffers or None,
        "last_passive_records": last.get("passive_records"),
        "last_passive_directory_bytes": last.get("passive_directory_logical"),
        "last_selector_run_bytes": last.get("selector_run_logical"),
        "last_ancestor_file_read_bytes": last.get("ancestor_file_reads_bytes"),
        "last_ancestor_file_write_bytes": last.get("ancestor_file_writes_bytes"),
        "signals": signals,
    }


def fmt(value, digits=2):
    if value is None:
        return "NA"
    if isinstance(value, str):
        return value
    if isinstance(value, int):
        return str(value)
    return ("{:,.%df}" % digits).format(value)


def markdown(label, rows, summary, total_samples=None):
    print("## " + label)
    print()
    print("All rates and CPU shares are interval deltas since the preceding "
          "periodic report; the first interval starts at process zero.")
    print()
    if total_samples is not None and total_samples != len(rows):
        print("Showing the last {} of {} periodic intervals.".format(
            len(rows), total_samples))
        print()
    print("| CPU s | Given | Given/CPU | Generated/given | Infer CPU % | "
          "Preprocess CPU % | Demod CPU % | PSS MiB | Swap MiB |")
    print("|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
    for row in rows:
        print("| {} | {} | {} | {} | {} | {} | {} | {} | {} |".format(
            fmt(row.get("user_cpu")), fmt(row.get("given")),
            fmt(row.get("given_per_cpu"), 3),
            fmt(row.get("generated_per_given"), 1),
            fmt(row.get("clock_infer_cpu_pct"), 1),
            fmt(row.get("clock_preprocess_cpu_pct"), 1),
            fmt(row.get("clock_demod_cpu_pct"), 1),
            fmt(row.get("pss_mib"), 1), fmt(row.get("swap_mib"), 1)))
    print()
    print("| CPU s | Back active | Groups/query | Nodes/query | "
          "Siblings/query | Child/query | Combined/query | Back CPU % | "
          "Child hit % | Back MiB |")
    print("|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
    for row in rows:
        print("| {} | {} | {} | {} | {} | {} | {} | {} | {} | {} |".format(
            fmt(row.get("user_cpu")), fmt(row.get("back_active")),
            fmt(row.get("back_groups_per_query"), 1),
            fmt(row.get("back_nodes_per_query"), 1),
            fmt(row.get("back_siblings_per_query"), 1),
            fmt(row.get("back_children_per_query"), 1),
            fmt(row.get("back_combined_per_query"), 1),
            fmt(row.get("back_lookup_cpu_pct"), 1),
            fmt(row.get("back_child_hit_pct"), 1),
            fmt(row.get("back_mib"), 1)))
    print()
    markdown_summary(summary)


def markdown_summary(summary):
    print("Final: strategy={}, {} givens, {} generated, {} kept, PSS={} MiB, "
          "back index={} MiB.".format(
              fmt(summary.get("strategy")), fmt(summary.get("last_given")),
              fmt(summary.get("last_generated")), fmt(summary.get("last_kept")),
              fmt(summary.get("last_pss_mib"), 1),
              fmt(summary.get("last_back_mib"), 1)))
    print("First-to-last interval ratios: counted back work/query={}, "
          "given/CPU={}.".format(
              fmt(summary.get("combined_slope_ratio"), 2),
              fmt(summary.get("given_rate_ratio"), 2)))
    if summary["signals"]:
        print("Signals:")
        for signal in summary["signals"]:
            print("- " + signal)
    else:
        print("Signals: none in the parsed counters (mature acceptance still "
              "requires a matched reference run).")
    print()


TSV_COLUMNS = (
    "label", "sample", "user_cpu", "system_cpu", "wall_clock", "given",
    "generated", "kept", "delta_given", "delta_generated", "delta_kept",
    "given_per_cpu", "generated_per_given", "back_strategy", "back_active",
    "delta_back_queries", "back_groups_per_query", "back_nodes_per_query",
    "back_siblings_per_query", "back_children_per_query",
    "back_combined_per_query", "back_lookup_cpu_pct", "back_child_hit_pct",
    "back_tree_child_parents", "back_tree_child_bytes", "back_bytes",
    "residency_pss", "residency_anonymous", "residency_swap",
    "passive_records", "passive_directory_logical", "selector_buffer_bytes",
    "selector_run_logical", "ancestor_file_reads_bytes",
    "ancestor_file_writes_bytes", "delta_ancestor_file_reads_bytes",
    "delta_ancestor_file_writes_bytes", "delta_selector_reads_bytes",
    "delta_selector_writes_bytes", "ancestor_io_mib_per_cpu",
    "selector_io_mib_per_cpu", "statistics_format_comma_num_buffers",
    "clock_infer", "clock_preprocess",
    "clock_demod", "clock_back_demod",
)


def emit_tsv(runs):
    print("\t".join(TSV_COLUMNS))
    for label, rows, unused_summary in runs:
        del unused_summary
        for row in rows:
            values = []
            for key in TSV_COLUMNS:
                value = label if key == "label" else row.get(key)
                values.append("" if value is None else str(value))
            print("\t".join(values))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("outputs", nargs="+", help="plain/.gz Prover9 output, or -")
    parser.add_argument("--format", choices=("markdown", "tsv", "json"),
                        default="markdown")
    parser.add_argument(
        "--tail", type=int, default=20,
        help="markdown only: show the last N intervals (0 means all; default 20)")
    parser.add_argument(
        "--summary-only", action="store_true",
        help="markdown only: omit interval tables")
    args = parser.parse_args(argv)
    if args.tail < 0:
        parser.error("--tail must be nonnegative")
    runs = []
    basenames = ["stdin" if path == "-" else os.path.basename(path)
                 for path in args.outputs]
    for path, basename in zip(args.outputs, basenames):
        label = (os.path.relpath(path) if path != "-" and
                 basenames.count(basename) > 1 else basename)
        rows = derive_intervals(parse_file(path))
        runs.append((label, rows, run_summary(label, rows)))
    if args.format == "tsv":
        emit_tsv(runs)
    elif args.format == "json":
        print(json.dumps({"runs": [
            {"label": label, "samples": rows, "summary": summary}
            for label, rows, summary in runs]}, indent=2, sort_keys=True))
    else:
        for label, rows, summary in runs:
            if args.summary_only:
                print("## " + label)
                print()
                markdown_summary(summary)
            else:
                shown = rows if args.tail == 0 else rows[-args.tail:]
                markdown(label, shown, summary, len(rows))
    return 0


if __name__ == "__main__":
    sys.exit(main())
