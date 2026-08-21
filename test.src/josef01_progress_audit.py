#!/usr/bin/env python3
"""Audit a growing Josef 01 compact-OTTER run from periodic statistics.

This is deliberately an interval tool.  Cumulative end-of-run counters can
hide a late collapse in CPU throughput, adaptive-unit selectivity, hint-cache
value, or selector I/O.  Plain and completed gzip-compressed Prover9 outputs
are accepted; point it at the still-growing plain output during a long run.
"""

import argparse
import json
import math
import os
import statistics
import sys

import compact_long_run_report as long_report


EXPECTED_ENDPOINT = (30827, 1602769536, 36195388, 1)
EXPECTED_MATCHED_HINTS = 48968
WINDOW = 3
WARMUP = 1
MIN_STEADY_INTERVALS = WARMUP + 2 * WINDOW

CUMULATIVE_KEYS = (
    "given", "generated", "kept", "user_cpu", "system_cpu",
    "rule_binary", "rule_hyper", "rule_ur", "rule_paramod", "rule_other",
    "unit_conflict_queries", "unit_conflict_exact_tests",
    "unit_position_queries", "unit_position_postings",
    "unit_code_tree_queries", "unit_code_tree_nodes",
    "unit_position_refinement_checks", "unit_position_refinement_rejects",
    "unit_position_tertiary_checks", "unit_position_tertiary_rejects",
    "hint_cache_queries", "hint_cache_hits", "hint_cache_misses",
    "hint_cache_stores", "hint_cache_admission_skips",
    "hint_cache_arena_wraps", "hint_cache_arena_expired_misses",
    "hint_cache_posting_candidates_avoided",
    "hint_conjunction_queries", "hint_conjunction_posting_candidates",
    "hint_conjunction_profile_rejects",
    "hint_conjunction_summary_reject_queries",
    "hint_conjunction_summary_reject_candidates",
    "selector_flushes", "selector_merges", "selector_reads_bytes",
    "selector_writes_bytes", "periodic_report_generated",
    "periodic_report_cpu_time_reads",
)


def number(mapping, key, default=None):
    value = mapping.get(key, default)
    return value if isinstance(value, (int, float)) else default


def ratio(numerator, denominator):
    if (not isinstance(numerator, (int, float)) or
            not isinstance(denominator, (int, float)) or denominator <= 0):
        return None
    return float(numerator) / float(denominator)


def percent(numerator, denominator):
    value = ratio(numerator, denominator)
    return None if value is None else value * 100.0


def finite(values):
    return [value for value in values
            if isinstance(value, (int, float)) and math.isfinite(value)]


def derive_intervals(samples):
    previous = {key: 0 for key in CUMULATIVE_KEYS}
    rows = []
    for ordinal, sample in enumerate(samples, 1):
        row = dict(sample)
        row["sample"] = ordinal
        row["counter_reset"] = False
        for key in CUMULATIVE_KEYS:
            current = number(sample, key)
            prior = number(previous, key)
            if current is None or prior is None:
                delta = None
            elif current < prior:
                delta = current
                row["counter_reset"] = True
            else:
                delta = current - prior
            row["delta_" + key] = delta

        delta_user = row.get("delta_user_cpu")
        delta_system = row.get("delta_system_cpu")
        row["delta_total_cpu"] = (
            delta_user + delta_system
            if isinstance(delta_user, (int, float)) and
            isinstance(delta_system, (int, float)) else None)
        row["total_cpu"] = (
            number(sample, "user_cpu", 0) + number(sample, "system_cpu", 0)
            if number(sample, "user_cpu") is not None and
            number(sample, "system_cpu") is not None else None)
        row["given_per_total_cpu"] = ratio(
            row.get("delta_given"), row.get("delta_total_cpu"))
        row["generated_per_total_cpu"] = ratio(
            row.get("delta_generated"), row.get("delta_total_cpu"))
        row["generated_per_given"] = ratio(
            row.get("delta_generated"), row.get("delta_given"))

        conflicts = row.get("delta_unit_conflict_queries")
        row["unit_tree_route_pct"] = percent(
            row.get("delta_unit_code_tree_queries"), conflicts)
        row["unit_position_route_pct"] = percent(
            row.get("delta_unit_position_queries"), conflicts)
        row["unit_tree_nodes_per_conflict"] = ratio(
            row.get("delta_unit_code_tree_nodes"), conflicts)
        row["unit_position_postings_per_conflict"] = ratio(
            row.get("delta_unit_position_postings"), conflicts)
        row["unit_exact_tests_per_conflict"] = ratio(
            row.get("delta_unit_conflict_exact_tests"), conflicts)
        row["unit_refinement_reject_pct"] = percent(
            row.get("delta_unit_position_refinement_rejects"),
            row.get("delta_unit_position_refinement_checks"))
        row["unit_tertiary_reject_pct"] = percent(
            row.get("delta_unit_position_tertiary_rejects"),
            row.get("delta_unit_position_tertiary_checks"))
        row["unit_feature_mib"] = ratio(
            number(sample, "unit_features"), 1024 * 1024)
        row["unit_index_mib"] = ratio(
            number(sample, "unit_bytes"), 1024 * 1024)

        cache_queries = row.get("delta_hint_cache_queries")
        cache_hits = row.get("delta_hint_cache_hits")
        row["hint_cache_hit_pct"] = percent(cache_hits, cache_queries)
        row["hint_cache_store_pct"] = percent(
            row.get("delta_hint_cache_stores"), cache_queries)
        row["hint_cache_avoided_per_hit"] = ratio(
            row.get("delta_hint_cache_posting_candidates_avoided"),
            cache_hits)
        row["hint_cache_expired_pct"] = percent(
            row.get("delta_hint_cache_arena_expired_misses"), cache_queries)
        row["hint_summary_reject_pct"] = percent(
            row.get("delta_hint_conjunction_summary_reject_candidates"),
            row.get("delta_hint_conjunction_posting_candidates"))
        row["hint_profile_reject_pct"] = percent(
            row.get("delta_hint_conjunction_profile_rejects"),
            row.get("delta_hint_conjunction_posting_candidates"))

        selector_io = (
            number(row, "delta_selector_reads_bytes", 0) +
            number(row, "delta_selector_writes_bytes", 0))
        row["selector_io_mib_per_cpu"] = ratio(
            selector_io,
            row["delta_total_cpu"] * 1024 * 1024
            if isinstance(row.get("delta_total_cpu"), (int, float)) else None)
        row["pss_mib"] = ratio(number(sample, "residency_pss"), 1024)
        row["swap_mib"] = ratio(number(sample, "residency_swap"), 1024)
        rows.append(row)
        previous = sample
    return rows


def complete_samples(samples):
    """Ignore a currently-being-written final framed statistics block."""
    if (samples and samples[-1].get("statistics_framed") and
            not samples[-1].get("statistics_complete")):
        return samples[:-1]
    return samples


def steady_ratio(rows, key, higher_is_better=False):
    values = [row.get(key) for row in rows]
    if len(values) < MIN_STEADY_INTERVALS or any(
            not isinstance(value, (int, float)) for value in values):
        return None
    early = statistics.median(values[WARMUP:WARMUP + WINDOW])
    tail = statistics.median(values[-WINDOW:])
    value = ratio(tail, early)
    if value is None:
        return None
    return (ratio(early, tail) if higher_is_better else value)


def config_signal(signals, actual, expected, description):
    if actual != expected:
        signals.append("{} is {!r}; expected {!r}".format(
            description, actual, expected))


def summarize(label, rows):
    if not rows:
        return {"label": label, "samples": 0,
                "signals": ["no periodic Given= statistics blocks found"]}
    last = rows[-1]
    signals = []
    if len(rows) < 2:
        signals.append("only one statistics block; interval slopes are unproven")
    elif len(rows) < MIN_STEADY_INTERVALS:
        signals.append(
            "post-warm-up slope gates need at least {} statistics blocks"
            .format(MIN_STEADY_INTERVALS))
    if any(row.get("counter_reset") for row in rows):
        signals.append("one or more cumulative counters reset between reports")
    if number(last, "residency_swap", 0) != 0:
        signals.append("process reports nonzero swap residency")
    if number(last, "unit_failures", 0) != 0:
        signals.append("compact unit index reports failures")

    config_signal(signals, last.get("search_mode"), "otter", "search loop")
    config_signal(signals, last.get("search_frontier"), "clauses",
                  "inference frontier")
    config_signal(signals, last.get("unit_strategy"), "adaptive",
                  "compact unit strategy")
    config_signal(signals, number(last, "unit_feature_depth"), 2,
                  "compact unit feature depth")
    config_signal(signals, last.get("hint_index_mode"), "packed_fast",
                  "hint index")
    config_signal(signals, last.get("hint_conjunction_enabled"), "yes",
                  "packed conjunction sidecar")
    config_signal(signals, last.get("passive_directory"), "file",
                  "passive directory")
    config_signal(signals, number(last, "passive_directory_entry_bytes"), 64,
                  "passive directory entry width")
    config_signal(signals, last.get("selector_store"), "file",
                  "selector store")
    config_signal(signals, number(last, "selector_buffer_limit"), 1048576,
                  "selector buffer limit")
    config_signal(signals, number(last, "selector_record_bits"), 64,
                  "selector record reference width")
    config_signal(signals, number(last, "selector_entry_bytes"), 16,
                  "selector entry width")
    config_signal(signals, number(last, "hint_cache_entries"), 16384,
                  "hint cache entries")
    config_signal(signals, number(last, "hint_cache_min_candidates"), 128,
                  "hint cache admission threshold")
    config_signal(signals, number(last, "hint_cache_overlap_invalidations"), 0,
                  "hint cache overlap invalidations")
    config_signal(signals, number(last, "periodic_report_interval"), 256,
                  "periodic report polling interval")
    config_signal(signals,
                  number(last, "statistics_format_comma_num_buffers"), 32,
                  "statistics formatting buffer count")
    if number(last, "residency_pss") is None:
        signals.append("process PSS telemetry is missing")
    if number(last, "residency_swap") is None:
        signals.append("process swap telemetry is missing")
    if any(number(last, "clock_" + name, 0) > 0 for name in
           ("infer", "preprocess", "hints", "subsume", "conflict")):
        signals.append(
            "detailed internal clocks are nonzero; use clear(clocks) for the "
            "authority throughput run")

    secondary_checks = number(
        last, "delta_unit_position_refinement_checks", 0)
    if (secondary_checks >= 1000 and
            number(last, "unit_refinement_reject_pct", 100) < 25):
        signals.append(
            "last-interval secondary unit refinement rejects under 25%")
    tertiary_checks = number(last, "delta_unit_position_tertiary_checks", 0)
    if (tertiary_checks >= 1000 and
            number(last, "unit_tertiary_reject_pct", 100) < 25):
        signals.append(
            "last-interval tertiary unit refinement rejects under 25%")

    throughput_slope = steady_ratio(
        rows, "given_per_total_cpu", higher_is_better=True)
    generated_slope = steady_ratio(
        rows, "generated_per_total_cpu", higher_is_better=True)
    unit_tree_slope = steady_ratio(rows, "unit_tree_nodes_per_conflict")
    unit_posting_slope = steady_ratio(
        rows, "unit_position_postings_per_conflict")
    unit_exact_slope = steady_ratio(rows, "unit_exact_tests_per_conflict")
    if throughput_slope is not None and throughput_slope > 1.25:
        signals.append(
            "post-warm-up given/total-CPU throughput fell by more than 20%")
    if generated_slope is not None and generated_slope > 1.25:
        signals.append(
            "post-warm-up generated/total-CPU throughput fell by more than 20%")
    for value, description in (
            (unit_tree_slope, "unit tree nodes/conflict"),
            (unit_posting_slope, "unit position postings/conflict"),
            (unit_exact_slope, "unit exact tests/conflict")):
        if value is not None and value > 1.25:
            signals.append(
                "post-warm-up {} grew by more than 25%".format(description))

    endpoint = tuple(number(last, key) for key in
                     ("given", "generated", "kept", "proofs"))
    endpoint_state = "in_progress"
    if number(last, "proofs", 0) > 0 or number(last, "given", 0) >= 30827:
        if endpoint == EXPECTED_ENDPOINT and number(
                last, "hint_matched") == EXPECTED_MATCHED_HINTS:
            endpoint_state = "exact_proof_endpoint"
        else:
            endpoint_state = "mismatch"
            signals.append(
                "terminal Josef trajectory does not match the authority "
                "endpoint and hint total")

    return {
        "label": label,
        "samples": len(rows),
        "endpoint_state": endpoint_state,
        "last_given": number(last, "given"),
        "last_generated": number(last, "generated"),
        "last_kept": number(last, "kept"),
        "last_proofs": number(last, "proofs"),
        "last_hint_matched": number(last, "hint_matched"),
        "last_total_cpu": number(last, "total_cpu"),
        "last_pss_mib": number(last, "pss_mib"),
        "last_swap_mib": number(last, "swap_mib"),
        "unit_strategy": last.get("unit_strategy"),
        "unit_feature_depth": number(last, "unit_feature_depth"),
        "last_unit_feature_mib": number(last, "unit_feature_mib"),
        "last_unit_index_mib": number(last, "unit_index_mib"),
        "last_refinement_reject_pct": number(
            last, "unit_refinement_reject_pct"),
        "last_tertiary_reject_pct": number(
            last, "unit_tertiary_reject_pct"),
        "last_hint_cache_hit_pct": number(last, "hint_cache_hit_pct"),
        "last_hint_summary_reject_pct": number(
            last, "hint_summary_reject_pct"),
        "last_selector_flushes": number(last, "selector_flushes"),
        "last_selector_merges": number(last, "selector_merges"),
        "throughput_slope_ratio": throughput_slope,
        "generated_slope_ratio": generated_slope,
        "unit_tree_slope_ratio": unit_tree_slope,
        "unit_posting_slope_ratio": unit_posting_slope,
        "unit_exact_slope_ratio": unit_exact_slope,
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


def markdown(label, rows, summary, total_rows):
    print("## " + label)
    print()
    if len(rows) != total_rows:
        print("Showing the last {} of {} statistics blocks.".format(
            len(rows), total_rows))
        print()
    print("| Total CPU s | Given | Given/CPU | Generated/CPU | Gen/given | "
          "PSS MiB | Swap MiB |")
    print("|---:|---:|---:|---:|---:|---:|---:|")
    for row in rows:
        print("| {} | {} | {} | {} | {} | {} | {} |".format(
            fmt(row.get("total_cpu")), fmt(row.get("given")),
            fmt(row.get("given_per_total_cpu"), 3),
            fmt(row.get("generated_per_total_cpu"), 1),
            fmt(row.get("generated_per_given"), 1),
            fmt(row.get("pss_mib"), 1), fmt(row.get("swap_mib"), 1)))
    print()
    print("| CPU s | Conflicts | Tree % | Position % | Tree nodes/query | "
          "Postings/query | Exact/query | Refine reject % | Tertiary reject % | "
          "Feature MiB |")
    print("|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
    for row in rows:
        print("| {} | {} | {} | {} | {} | {} | {} | {} | {} | {} |".format(
            fmt(row.get("total_cpu")),
            fmt(row.get("delta_unit_conflict_queries")),
            fmt(row.get("unit_tree_route_pct"), 1),
            fmt(row.get("unit_position_route_pct"), 1),
            fmt(row.get("unit_tree_nodes_per_conflict"), 2),
            fmt(row.get("unit_position_postings_per_conflict"), 2),
            fmt(row.get("unit_exact_tests_per_conflict"), 3),
            fmt(row.get("unit_refinement_reject_pct"), 1),
            fmt(row.get("unit_tertiary_reject_pct"), 1),
            fmt(row.get("unit_feature_mib"), 1)))
    print()
    print("| CPU s | Hint cache hit % | Store % | Avoided/hit | "
          "Summary reject % | Profile reject % | Selector flushes | "
          "Selector merges | Selector I/O MiB/CPU s |")
    print("|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
    for row in rows:
        print("| {} | {} | {} | {} | {} | {} | {} | {} | {} |".format(
            fmt(row.get("total_cpu")),
            fmt(row.get("hint_cache_hit_pct"), 2),
            fmt(row.get("hint_cache_store_pct"), 2),
            fmt(row.get("hint_cache_avoided_per_hit"), 1),
            fmt(row.get("hint_summary_reject_pct"), 1),
            fmt(row.get("hint_profile_reject_pct"), 1),
            fmt(row.get("delta_selector_flushes")),
            fmt(row.get("delta_selector_merges")),
            fmt(row.get("selector_io_mib_per_cpu"), 2)))
    print()
    print("Final: state={}, endpoint=({}, {}, {}, proofs={}), hints={}, "
          "unit={}/depth {}, unit feature/index={}/{} MiB, PSS={} MiB.".format(
              summary["endpoint_state"], fmt(summary["last_given"]),
              fmt(summary["last_generated"]), fmt(summary["last_kept"]),
              fmt(summary["last_proofs"]),
              fmt(summary["last_hint_matched"]),
              fmt(summary["unit_strategy"]),
              fmt(summary["unit_feature_depth"]),
              fmt(summary["last_unit_feature_mib"], 1),
              fmt(summary["last_unit_index_mib"], 1),
              fmt(summary["last_pss_mib"], 1)))
    print("Post-warm-up gate ratios (over 1.25 warns): given throughput={}, "
          "generated throughput={}, tree work={}, posting work={}, exact "
          "work={}.".format(
              fmt(summary["throughput_slope_ratio"], 2),
              fmt(summary["generated_slope_ratio"], 2),
              fmt(summary["unit_tree_slope_ratio"], 2),
              fmt(summary["unit_posting_slope_ratio"], 2),
              fmt(summary["unit_exact_slope_ratio"], 2)))
    if summary["signals"]:
        print("Signals:")
        for signal in summary["signals"]:
            print("- " + signal)
    else:
        print("Signals: none; proof-endpoint acceptance is still required.")
    print()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("outputs", nargs="+", help="plain or .gz Prover9 output")
    parser.add_argument("--tail", type=int, default=12,
                        help="show the last N blocks (0 means all; default 12)")
    parser.add_argument("--json", action="store_true",
                        help="emit machine-readable JSON")
    args = parser.parse_args(argv)
    if args.tail < 0:
        parser.error("--tail must be nonnegative")
    runs = []
    for path in args.outputs:
        rows = derive_intervals(complete_samples(long_report.parse_file(path)))
        label = os.path.basename(path)
        runs.append({"label": label, "rows": rows,
                     "summary": summarize(label, rows)})
    if args.json:
        print(json.dumps({"runs": runs}, indent=2, sort_keys=True))
    else:
        for run in runs:
            all_rows = run["rows"]
            shown = all_rows if args.tail == 0 else all_rows[-args.tail:]
            markdown(run["label"], shown, run["summary"], len(all_rows))
    return 0


if __name__ == "__main__":
    sys.exit(main())
