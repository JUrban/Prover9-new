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
import statistics
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
RSS_KB_RE = re.compile(
    r"RSS_kb:\s*current=([0-9,]+),\s*peak=([0-9,]+)")
GNU_TIME_USER_RE = re.compile(
    r"^\s*User time \(seconds\):\s*([0-9]+(?:\.[0-9]+)?)\s*$")
GNU_TIME_RSS_RE = re.compile(
    r"^\s*Maximum resident set size \(kbytes\):\s*([0-9]+)\s*$")
DEMOD_RE = re.compile(
    r"^Demod_attempts=([0-9]+)\. Demod_rewrites=([0-9]+)\.$")

PREFIXES = (
    ("Compact_back_demod:", "back"),
    ("Compact_back_edge:", "back_edge"),
    ("Compact_back_route:", "back_route"),
    ("Dense_passive:", "passive"),
    ("Dense_passive_selector:", "selector"),
    ("Dense_passive_gc:", "passive_gc"),
    ("Ancestor_store:", "ancestor"),
    ("Process_residency_kb:", "residency"),
    ("Compact_otter_demodulation:", "rewrite"),
    ("Compact_rewrite_shape:", "rewrite_shape"),
    ("Compact_rewrite_root_cache:", "rewrite_root_cache"),
    ("Compact_rewrite_deep_cache:", "rewrite_deep_cache"),
    ("Compact_rewrite_subjects:", "rewrite_subject"),
)

STATISTICS_FORMAT_MARKER = "Statistics_format:"
SAFE_COMMA_NUM_BUFFERS = 24
STEADY_WARMUP_INTERVALS = 1
STEADY_WINDOW_INTERVALS = 3
MIN_STEADY_INTERVALS = (
    STEADY_WARMUP_INTERVALS + 2 * STEADY_WINDOW_INTERVALS)
MIN_SAMPLED_LOOKUP_TIMINGS_PER_INTERVAL = 32

CUMULATIVE_KEYS = (
    "given", "generated", "kept", "user_cpu", "system_cpu",
    "back_queries", "back_groups_examined", "back_tree_nodes_examined",
    "back_mask_trie_queries", "back_mask_trie_nodes_examined",
    "back_mask_trie_prunes",
    "back_mask_directory_queries", "back_mask_directory_blocks_examined",
    "back_mask_directory_word_checks",
    "back_mask_directory_buckets_selected",
    "back_tree_sibling_checks", "back_tree_child_lookups",
    "back_tree_insert_sibling_checks", "back_tree_insert_cache_lookups",
    "back_tree_insert_cache_hits", "back_tree_insert_cache_misses",
    "back_candidates", "back_timing_lookup_seconds",
    "back_timing_lookup_samples",
    "back_timing_exact_seconds", "back_timing_materialize_seconds",
    "back_timing_maintenance_seconds", "demod_attempts", "demod_rewrites",
    "back_profile_exact_tests", "back_profile_exact_successes",
    "back_route_collisions", "back_route_replacements",
    "back_route_profile_hits", "back_route_profile_misses",
    "back_route_cold_fallbacks", "back_route_admission_attempts",
    "back_route_admission_rejections", "back_route_aged_replacements",
    "back_route_frequency_decays",
    "back_route_pre_tree_observations",
    "back_route_pre_tree_hot_observations",
    "back_route_mask_choices", "back_route_tree_choices",
    "back_route_position_choices", "back_route_mask_probes",
    "back_route_tree_probes", "back_route_position_probes",
    "back_route_tree_probe_aborts", "back_route_tree_probe_budget",
    "back_route_tree_probe_discarded_candidates",
    "back_route_switches", "back_route_reversions",
    "back_route_hysteresis_holds", "back_route_mask_observed_cost",
    "back_route_tree_observed_cost", "back_route_position_observed_cost",
    "back_route_mask_estimated_cost", "back_route_tree_estimated_cost",
    "back_route_position_estimated_cost", "back_route_mask_candidates",
    "back_route_tree_candidates", "back_route_position_candidates",
    "back_position_admissions", "back_position_rejections",
    "back_position_demotions", "back_position_retry_deferrals",
    "back_position_census_records", "back_position_backfill_records",
    "back_position_append_records", "back_position_append_root_scans",
    "back_position_append_token_visits",
    "back_position_append_feature_lookups",
    "back_position_append_matches",
    "back_position_credit_earned", "back_position_credit_spent",
    "back_position_credit_reservations",
    "back_position_admission_freezes",
    "back_edge_queries", "back_edge_empty_queries",
    "back_edge_bypass_queries", "back_edge_intersection_queries",
    "back_edge_query_features", "back_edge_selected_features",
    "back_edge_posting_records", "back_edge_candidate_records",
    "back_edge_exact_rejects", "back_edge_append_records",
    "back_edge_append_token_visits", "back_edge_append_feature_lookups",
    "rewrite_deep_cache_lookups", "rewrite_deep_cache_hits",
    "rewrite_deep_cache_misses", "rewrite_deep_cache_replacements",
    "rewrite_deep_cache_growth_denials",
    "rewrite_deep_cache_variable_sibling_checks",
    "rewrite_deep_cache_rigid_sibling_checks",
    "rewrite_subject_atoms", "rewrite_subject_initial_nodes",
    "rewrite_subject_target_nodes", "rewrite_subject_attempts",
    "clock_infer", "clock_preprocess", "clock_demod", "clock_hints",
    "clock_subsume", "clock_back_demod", "ancestor_file_reads_bytes",
    "ancestor_file_writes_bytes", "selector_reads_bytes",
    "selector_writes_bytes", "selector_read_evictions",
    "selector_read_evictions_bytes", "selector_read_eviction_failures",
    "selector_min_calls", "selector_buffer_checks", "selector_run_checks",
)


def scalar(text):
    """Return an int/float when the complete leading scalar is numeric."""
    text = text.strip().rstrip(".")
    match = NUMBER_RE.match(text)
    if match and match.end() == len(text):
        number = match.group(1)
        return float(number) if "." in number else int(number)
    return text


def integer_with_commas(text):
    return int(text.replace(",", ""))


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
        demod_match = DEMOD_RE.match(line)
        if demod_match:
            sample["demod_attempts"] = int(demod_match.group(1))
            sample["demod_rewrites"] = int(demod_match.group(2))
            continue
        if line.startswith("Megabytes="):
            sample["reported_megabytes"] = scalar(line.split("=", 1)[1])
            continue
        rss_match = RSS_KB_RE.search(line)
        if rss_match:
            sample["allocator_rss_current_kb"] = integer_with_commas(
                rss_match.group(1))
            sample["allocator_rss_peak_kb"] = integer_with_commas(
                rss_match.group(2))
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


def inferred_time_sidecar(path):
    if path == "-":
        return None
    base = path[:-3] if path.endswith(".gz") else path
    if base.endswith(".out"):
        base = base[:-4]
    candidates = (base + ".time", path + ".time")
    for candidate in candidates:
        if os.path.isfile(candidate):
            return candidate
    return None


def inferred_cgroup_sidecar(path):
    if path == "-":
        return None
    base = path[:-3] if path.endswith(".gz") else path
    if base.endswith(".out"):
        base = base[:-4]
    candidates = (base + ".cgroup", path + ".cgroup")
    for candidate in candidates:
        if os.path.isfile(candidate):
            return candidate
    return None


def parse_gnu_time(path):
    result = {"external_time_file": path}
    with open(path, "r", encoding="utf-8", errors="replace") as stream:
        for line in stream:
            match = GNU_TIME_USER_RE.match(line)
            if match:
                result["external_user_cpu"] = float(match.group(1))
                continue
            match = GNU_TIME_RSS_RE.match(line)
            if match:
                result["external_peak_rss_kb"] = int(match.group(1))
    return result


def parse_cgroup_report(path):
    result = {"cgroup_report_file": path}
    with open(path, "r", encoding="utf-8", errors="replace") as stream:
        for line in stream:
            if "=" not in line:
                continue
            key, value = line.rstrip("\n").split("=", 1)
            result["cgroup_" + key] = scalar(value)
    return result


def parse_run(path):
    samples = parse_file(path)
    sidecar = inferred_time_sidecar(path)
    if samples and sidecar is not None:
        samples[-1].update(parse_gnu_time(sidecar))
    sidecar = inferred_cgroup_sidecar(path)
    if samples and sidecar is not None:
        samples[-1].update(parse_cgroup_report(sidecar))
    return samples


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
        delta_mask_nodes = row.get("delta_back_mask_trie_nodes_examined")
        delta_mask_blocks = row.get(
            "delta_back_mask_directory_blocks_examined")
        delta_mask_words = row.get("delta_back_mask_directory_word_checks")
        mask_components = (delta_mask_nodes, delta_mask_blocks,
                           delta_mask_words)
        delta_mask_work = (
            sum(value for value in mask_components if value is not None)
            if any(value is not None for value in mask_components) else None)
        delta_nodes = row.get("delta_back_tree_nodes_examined")
        delta_siblings = row.get("delta_back_tree_sibling_checks")
        delta_children = row.get("delta_back_tree_child_lookups")
        components = (delta_groups, delta_mask_work, delta_nodes,
                      delta_siblings,
                      delta_children)
        combined = (sum(value for value in components if value is not None)
                    if any(value is not None for value in components) else None)
        row["delta_back_combined_work"] = combined
        row["given_per_cpu"] = safe_ratio(delta_given, delta_cpu)
        row["generated_per_given"] = safe_ratio(
            row.get("delta_generated"), delta_given)
        row["kept_per_given"] = safe_ratio(row.get("delta_kept"), delta_given)
        row["back_groups_per_query"] = safe_ratio(delta_groups, delta_queries)
        row["back_mask_nodes_per_query"] = safe_ratio(
            delta_mask_work, delta_queries)
        row["back_nodes_per_query"] = safe_ratio(delta_nodes, delta_queries)
        row["back_siblings_per_query"] = safe_ratio(
            delta_siblings, delta_queries)
        row["back_children_per_query"] = safe_ratio(
            delta_children, delta_queries)
        row["back_combined_per_query"] = safe_ratio(combined, delta_queries)
        row["back_edge_posting_records_per_query"] = safe_ratio(
            row.get("delta_back_edge_posting_records"), delta_queries)
        row["back_edge_candidates_per_query"] = safe_ratio(
            row.get("delta_back_edge_candidate_records"), delta_queries)
        row["back_edge_exact_reject_pct"] = safe_ratio(
            row.get("delta_back_edge_exact_rejects"),
            row.get("delta_back_edge_candidate_records"))
        if row["back_edge_exact_reject_pct"] is not None:
            row["back_edge_exact_reject_pct"] *= 100.0
        row["back_lookup_cpu_pct"] = (None if delta_cpu is None else
            safe_ratio(row.get("delta_back_timing_lookup_seconds"), delta_cpu))
        if row["back_lookup_cpu_pct"] is not None:
            row["back_lookup_cpu_pct"] *= 100.0
        row["back_lookup_seconds_per_query"] = safe_ratio(
            row.get("delta_back_timing_lookup_seconds"), delta_queries)
        sampled_lookup = sample.get("back_timing_lookup_timing") == "sampled"
        row["back_lookup_timing_samples"] = row.get(
            "delta_back_timing_lookup_samples")
        row["back_lookup_timing_sufficient"] = (
            not sampled_lookup or
            number(row, "back_lookup_timing_samples", 0) >=
            MIN_SAMPLED_LOOKUP_TIMINGS_PER_INTERVAL)
        if not row["back_lookup_timing_sufficient"]:
            row["back_lookup_cpu_pct"] = None
            row["back_lookup_seconds_per_query"] = None
        row["back_lookup_us_per_query"] = (
            row["back_lookup_seconds_per_query"] * 1000000.0
            if row["back_lookup_seconds_per_query"] is not None else None)
        delta_exact_successes = row.get("delta_back_profile_exact_successes")
        row["back_exact_successes_per_query"] = safe_ratio(
            delta_exact_successes, delta_queries)
        route_choices = sum(number(row, "delta_back_route_" + route +
                                   "_choices", 0)
                            for route in ("mask", "tree", "position"))
        row["back_route_choices"] = route_choices
        for route in ("mask", "tree", "position"):
            choices = row.get("delta_back_route_" + route + "_choices")
            row["back_route_" + route + "_choice_pct"] = safe_ratio(
                choices, route_choices)
            if row["back_route_" + route + "_choice_pct"] is not None:
                row["back_route_" + route + "_choice_pct"] *= 100.0
            row["back_route_" + route + "_cost_per_choice"] = safe_ratio(
                row.get("delta_back_route_" + route + "_observed_cost"),
                choices)
            row["back_route_" + route + "_candidates_per_choice"] = safe_ratio(
                row.get("delta_back_route_" + route + "_candidates"),
                choices)
        answer_units = (
            delta_queries + delta_exact_successes
            if isinstance(delta_queries, (int, float)) and
            isinstance(delta_exact_successes, (int, float)) else None)
        row["back_lookup_seconds_per_answer_unit"] = safe_ratio(
            row.get("delta_back_timing_lookup_seconds"), answer_units)
        if not row["back_lookup_timing_sufficient"]:
            row["back_lookup_seconds_per_answer_unit"] = None
        row["back_lookup_us_per_answer_unit"] = (
            row["back_lookup_seconds_per_answer_unit"] * 1000000.0
            if row["back_lookup_seconds_per_answer_unit"] is not None
            else None)
        row["rewrite_deep_lookups_per_demod_attempt"] = safe_ratio(
            row.get("delta_rewrite_deep_cache_lookups"),
            row.get("delta_demod_attempts"))
        row["rewrite_rigid_sibling_checks_per_demod_attempt"] = safe_ratio(
            row.get("delta_rewrite_deep_cache_rigid_sibling_checks"),
            row.get("delta_demod_attempts"))
        row["rewrite_initial_nodes_per_atom"] = safe_ratio(
            row.get("delta_rewrite_subject_initial_nodes"),
            row.get("delta_rewrite_subject_atoms"))
        row["rewrite_attempts_per_atom"] = safe_ratio(
            row.get("delta_rewrite_subject_attempts"),
            row.get("delta_rewrite_subject_atoms"))
        row["rewrite_target_nodes_per_attempt"] = safe_ratio(
            row.get("delta_rewrite_subject_target_nodes"),
            row.get("delta_rewrite_subject_attempts"))
        row["rewrite_query_preparation_reduction"] = safe_ratio(
            row.get("delta_rewrite_subject_target_nodes"),
            row.get("delta_rewrite_subject_initial_nodes"))
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
        row["selector_read_eviction_pct"] = safe_ratio(
            row.get("delta_selector_read_evictions_bytes"),
            row.get("delta_selector_reads_bytes"))
        if row["selector_read_eviction_pct"] is not None:
            row["selector_read_eviction_pct"] *= 100.0
        row["selector_min_calls_per_given"] = safe_ratio(
            row.get("delta_selector_min_calls"), delta_given)
        row["selector_run_checks_per_given"] = safe_ratio(
            row.get("delta_selector_run_checks"), delta_given)
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
    back_cpu_costs = finite(
        [row.get("back_lookup_seconds_per_query") for row in rows])
    normalized_back_cpu_costs = finite(
        [row.get("back_lookup_seconds_per_answer_unit") for row in rows])
    normalized_evidence_complete = (
        len(normalized_back_cpu_costs) == len(rows))
    steady_early_median = None
    steady_tail_median = None
    steady_growth_ratio = None
    steady_tail_spread_ratio = None
    steady_slope_ratio = None
    if (normalized_evidence_complete and
            len(normalized_back_cpu_costs) >= STEADY_WINDOW_INTERVALS):
        tail_window = normalized_back_cpu_costs[-STEADY_WINDOW_INTERVALS:]
        steady_tail_median = statistics.median(tail_window)
        steady_tail_spread_ratio = safe_ratio(
            max(tail_window), min(tail_window))
    if (normalized_evidence_complete and
            len(normalized_back_cpu_costs) >= MIN_STEADY_INTERVALS):
        early_start = STEADY_WARMUP_INTERVALS
        early_end = early_start + STEADY_WINDOW_INTERVALS
        early_window = normalized_back_cpu_costs[early_start:early_end]
        steady_early_median = statistics.median(early_window)
        steady_growth_ratio = safe_ratio(
            steady_tail_median, steady_early_median)
        if (steady_growth_ratio is not None and
                steady_tail_spread_ratio is not None):
            steady_slope_ratio = max(
                steady_growth_ratio, steady_tail_spread_ratio)
    given_rates = finite([row.get("given_per_cpu") for row in rows])
    signals = []
    if len(rows) < 2:
        signals.append("only one periodic sample; interval slope is unproven")
    if steady_slope_ratio is None:
        signals.append(
            "steady back-lookup slope needs at least {} complete periodic "
            "intervals".format(MIN_STEADY_INTERVALS))
    insufficient_timing_intervals = sum(
        1 for row in rows
        if row.get("back_timing_lookup_timing") == "sampled" and
        not row.get("back_lookup_timing_sufficient"))
    if insufficient_timing_intervals:
        signals.append(
            "{} sampled back-lookup interval(s) have fewer than {} timing "
            "samples and are excluded from CPU slopes".format(
                insufficient_timing_intervals,
                MIN_SAMPLED_LOOKUP_TIMINGS_PER_INTERVAL))
    if number(last, "back_failures", 0) != 0:
        signals.append("compact backward-demodulation reports failures")
    if number(last, "passive_gc_validation_failures", 0) != 0:
        signals.append("dense-passive validation failures are nonzero")
    if number(last, "ancestor_validation_failures", 0) != 0:
        signals.append("ancestor-store validation failures are nonzero")
    if number(last, "residency_swap", 0) != 0:
        signals.append("process reports nonzero swap residency")
    selector_read_eviction_pct = safe_ratio(
        number(last, "selector_read_evictions_bytes", None),
        number(last, "selector_reads_bytes", None))
    if selector_read_eviction_pct is not None:
        selector_read_eviction_pct *= 100.0
    if (last.get("selector_store") == "file" and
            number(last, "selector_reads_bytes", 0) > 0 and
            (selector_read_eviction_pct is None or
             selector_read_eviction_pct < 99.0)):
        signals.append(
            "file selector does not advise at least 99% of consumed read "
            "bytes out of cache")
    if number(last, "selector_read_eviction_failures", 0) != 0:
        signals.append("file selector reports consumed-read eviction failures")
    if (last.get("selector_store") == "file" and
            (number(last, "selector_record_bits", 0) < 64 or
             number(last, "selector_entry_bytes", 0) > 24)):
        signals.append(
            "file selector lacks a 64-bit record reference in a 24-byte "
            "entry")
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
    if (len(back_cpu_costs) >= 2 and back_cpu_costs[0] > 0 and
            back_cpu_costs[-1] > back_cpu_costs[0] * 1.25):
        signals.append("back-demod lookup CPU/query grew by more than 25%")
    if (len(normalized_back_cpu_costs) >= 2 and
            normalized_back_cpu_costs[0] > 0 and
            normalized_back_cpu_costs[-1] >
            normalized_back_cpu_costs[0] * 1.25):
        signals.append(
            "back-demod lookup CPU/(query + exact answer) grew by more "
            "than 25%")
    if steady_slope_ratio is not None and steady_slope_ratio > 1.25:
        signals.append(
            "post-warm-up back-demod CPU is growing or its tail has not "
            "stabilized within 25%")
    if len(given_rates) >= 2 and given_rates[0] > 0 and given_rates[-1] < given_rates[0] * 0.75:
        signals.append("given/user-CPU rate fell by more than 25%")
    peak_rss_kb = safe_ratio(
        number(last, "cgroup_memory_peak_bytes", None), 1024)
    peak_rss_source = "cgroup v2 total-job peak" if peak_rss_kb is not None else None
    if peak_rss_kb is None:
        peak_rss_kb = number(last, "external_peak_rss_kb", None)
        if peak_rss_kb is not None:
            peak_rss_source = "GNU time sidecar"
    if peak_rss_kb is None:
        peak_rss_kb = number(last, "allocator_rss_peak_kb", None)
        if peak_rss_kb is not None:
            peak_rss_source = "Prover9 allocator report"
    last_user_cpu = number(last, "user_cpu", None)
    if last_user_cpu is None:
        last_user_cpu = number(last, "external_user_cpu", None)
    return {
        "label": label,
        "samples": len(rows),
        "first_user_cpu": first.get("user_cpu"),
        "last_user_cpu": last_user_cpu,
        "first_given": first.get("given"),
        "last_given": last.get("given"),
        "last_generated": last.get("generated"),
        "last_kept": last.get("kept"),
        "last_proofs": last.get("proofs"),
        "strategy": last.get("back_strategy"),
        "first_back_active": first.get("back_active"),
        "last_back_active": last.get("back_active"),
        "first_combined_per_query": combined[0] if combined else None,
        "last_combined_per_query": combined[-1] if combined else None,
        "combined_slope_ratio": (safe_ratio(combined[-1], combined[0])
                                 if combined else None),
        "first_back_lookup_seconds_per_query": (
            back_cpu_costs[0] if back_cpu_costs else None),
        "last_back_lookup_seconds_per_query": (
            back_cpu_costs[-1] if back_cpu_costs else None),
        "back_lookup_cpu_slope_ratio": (
            safe_ratio(back_cpu_costs[-1], back_cpu_costs[0])
            if back_cpu_costs else None),
        "first_back_lookup_seconds_per_answer_unit": (
            normalized_back_cpu_costs[0]
            if normalized_back_cpu_costs else None),
        "last_back_lookup_seconds_per_answer_unit": (
            normalized_back_cpu_costs[-1]
            if normalized_back_cpu_costs else None),
        "back_lookup_normalized_cpu_slope_ratio": (
            safe_ratio(normalized_back_cpu_costs[-1],
                       normalized_back_cpu_costs[0])
            if normalized_back_cpu_costs else None),
        "back_lookup_normalized_cpu_samples": len(normalized_back_cpu_costs),
        "back_lookup_normalized_early_median_seconds": steady_early_median,
        "back_lookup_normalized_tail_median_seconds": steady_tail_median,
        "back_lookup_normalized_steady_growth_ratio": steady_growth_ratio,
        "back_lookup_normalized_tail_spread_ratio": (
            steady_tail_spread_ratio),
        "back_lookup_normalized_steady_slope_ratio": steady_slope_ratio,
        "first_given_per_cpu": given_rates[0] if given_rates else None,
        "last_given_per_cpu": given_rates[-1] if given_rates else None,
        "given_rate_ratio": (safe_ratio(given_rates[-1], given_rates[0])
                             if given_rates else None),
        "last_back_lookup_cpu_pct": last.get("back_lookup_cpu_pct"),
        "last_pss_mib": last.get("pss_mib"),
        "last_anonymous_mib": last.get("anonymous_mib"),
        "last_swap_mib": last.get("swap_mib"),
        "last_back_mib": last.get("back_mib"),
        "last_back_route_profile_occupied": last.get(
            "back_route_occupied"),
        "last_back_route_profile_capacity": last.get(
            "back_route_capacity"),
        "last_back_route_profile_bytes": last.get("back_route_bytes"),
        "last_back_route_mask_choice_pct": last.get(
            "back_route_mask_choice_pct"),
        "last_back_route_tree_choice_pct": last.get(
            "back_route_tree_choice_pct"),
        "last_back_route_position_choice_pct": last.get(
            "back_route_position_choice_pct"),
        "last_back_route_switches": last.get("back_route_switches"),
        "last_back_route_reversions": last.get("back_route_reversions"),
        "last_back_route_cold_fallbacks": last.get(
            "back_route_cold_fallbacks"),
        "last_back_route_admission_rejections": last.get(
            "back_route_admission_rejections"),
        "last_back_route_frequency_decays": last.get(
            "back_route_frequency_decays"),
        "last_back_position_features": last.get("back_position_features"),
        "last_back_position_census_records": last.get(
            "back_position_census_records"),
        "last_back_position_backfill_records": last.get(
            "back_position_backfill_records"),
        "last_back_position_append_records": last.get(
            "back_position_append_records"),
        "last_back_position_append_token_visits": last.get(
            "back_position_append_token_visits"),
        "last_back_position_credit_earned": last.get(
            "back_position_credit_earned"),
        "last_back_position_credit_spent": last.get(
            "back_position_credit_spent"),
        "last_back_position_admission_frozen": last.get(
            "back_position_admission_frozen"),
        "last_child_hit_pct": last.get("back_child_hit_pct"),
        "peak_rss_kb": peak_rss_kb,
        "peak_rss_mib": safe_ratio(peak_rss_kb, 1024),
        "peak_rss_source": peak_rss_source,
        "reported_megabytes": last.get("reported_megabytes"),
        "cgroup_memory_peak_bytes": last.get("cgroup_memory_peak_bytes"),
        "cgroup_memory_current_bytes": last.get("cgroup_memory_current_bytes"),
        "cgroup_anon_current_bytes": last.get("cgroup_anon_current_bytes"),
        "cgroup_file_current_bytes": last.get("cgroup_file_current_bytes"),
        "cgroup_swap_peak_bytes": last.get("cgroup_swap_peak_bytes"),
        "statistics_format_buffers": formatting_buffers or None,
        "last_back_lookup_timing": last.get("back_timing_lookup_timing"),
        "last_back_lookup_rate": last.get("back_timing_lookup_rate"),
        "last_back_lookup_samples": last.get("back_timing_lookup_samples"),
        "last_passive_records": last.get("passive_records"),
        "last_passive_directory_bytes": last.get("passive_directory_logical"),
        "last_selector_run_bytes": last.get("selector_run_logical"),
        "last_selector_store": last.get("selector_store"),
        "last_selector_record_bits": last.get("selector_record_bits"),
        "last_selector_entry_bytes": last.get("selector_entry_bytes"),
        "last_selector_read_bytes": last.get("selector_reads_bytes"),
        "last_selector_read_eviction_pct": selector_read_eviction_pct,
        "last_selector_read_eviction_failures": last.get(
            "selector_read_eviction_failures"),
        "last_selector_min_calls_per_given": last.get(
            "selector_min_calls_per_given"),
        "last_selector_run_checks_per_given": last.get(
            "selector_run_checks_per_given"),
        "last_ancestor_file_read_bytes": last.get("ancestor_file_reads_bytes"),
        "last_ancestor_file_write_bytes": last.get("ancestor_file_writes_bytes"),
        "last_rewrite_query_preparation_reduction": safe_ratio(
            last.get("rewrite_subject_target_nodes"),
            last.get("rewrite_subject_initial_nodes")),
        "signals": signals,
    }


def threshold_state(value):
    if value is True:
        return "pass"
    if value is False:
        return "fail"
    return "unknown"


def compare_summaries(reference, candidate, max_cpu_ratio=1.25,
                      min_ram_saving_pct=80.0,
                      max_back_slope_ratio=1.25):
    trajectory_fields = ("last_given", "last_generated", "last_kept",
                         "last_proofs")
    trajectory_known = all(
        reference.get(key) is not None and candidate.get(key) is not None
        for key in trajectory_fields)
    trajectory_match = (all(reference.get(key) == candidate.get(key)
                            for key in trajectory_fields)
                        if trajectory_known else None)

    cpu_ratio = safe_ratio(candidate.get("last_user_cpu"),
                           reference.get("last_user_cpu"))
    cpu_gate = (cpu_ratio <= max_cpu_ratio
                if cpu_ratio is not None else None)
    rss_ratio = safe_ratio(candidate.get("peak_rss_kb"),
                           reference.get("peak_rss_kb"))
    ram_savings_pct = ((1.0 - rss_ratio) * 100.0
                       if rss_ratio is not None else None)
    ram_gate = (ram_savings_pct >= min_ram_saving_pct
                if ram_savings_pct is not None else None)
    normalized_samples = candidate.get(
        "back_lookup_normalized_cpu_samples", 0)
    slope_ratio = candidate.get(
        "back_lookup_normalized_steady_slope_ratio")
    interval_gate = (
        True if normalized_samples >= MIN_STEADY_INTERVALS and
        slope_ratio is not None else None)
    if interval_gate is None:
        slope_ratio = None
    slope_gate = (slope_ratio <= max_back_slope_ratio
                  if slope_ratio is not None else None)
    selector_store = candidate.get("last_selector_store")
    if selector_store == "heap":
        selector_cache_gate = True
    elif selector_store == "file":
        selector_read_bytes = candidate.get("last_selector_read_bytes")
        selector_coverage = candidate.get(
            "last_selector_read_eviction_pct")
        selector_failures = candidate.get(
            "last_selector_read_eviction_failures")
        selector_cache_gate = (
            selector_coverage >= 99.0 and selector_failures == 0
            if isinstance(selector_read_bytes, (int, float)) and
            selector_read_bytes > 0 and
            isinstance(selector_coverage, (int, float)) and
            isinstance(selector_failures, (int, float)) else None)
    else:
        selector_cache_gate = None
    if selector_store == "heap":
        selector_width_gate = True
    elif selector_store == "file":
        selector_bits = candidate.get("last_selector_record_bits")
        selector_entry_bytes = candidate.get("last_selector_entry_bytes")
        selector_width_gate = (
            selector_bits >= 64 and selector_entry_bytes <= 24
            if isinstance(selector_bits, (int, float)) and
            isinstance(selector_entry_bytes, (int, float)) else None)
    else:
        selector_width_gate = None

    checks = (trajectory_match, cpu_gate, ram_gate, interval_gate, slope_gate,
              selector_cache_gate, selector_width_gate)
    if any(value is False for value in checks):
        result = "reject"
    elif any(value is None for value in checks):
        result = "incomplete"
    else:
        result = "eligible"
    return {
        "reference": reference.get("label"),
        "candidate": candidate.get("label"),
        "result": result,
        "max_cpu_ratio": max_cpu_ratio,
        "min_ram_saving_pct": min_ram_saving_pct,
        "max_back_slope_ratio": max_back_slope_ratio,
        "trajectory_match": trajectory_match,
        "trajectory_gate": threshold_state(trajectory_match),
        "cpu_ratio": cpu_ratio,
        "cpu_gate": threshold_state(cpu_gate),
        "reference_peak_rss_mib": reference.get("peak_rss_mib"),
        "candidate_peak_rss_mib": candidate.get("peak_rss_mib"),
        "rss_ratio": rss_ratio,
        "ram_savings_pct": ram_savings_pct,
        "ram_gate": threshold_state(ram_gate),
        "candidate_periodic_samples": candidate.get("samples"),
        "candidate_normalized_samples": normalized_samples,
        "required_slope_samples": MIN_STEADY_INTERVALS,
        "interval_gate": threshold_state(interval_gate),
        "candidate_back_normalized_cpu_slope_ratio": slope_ratio,
        "slope_gate": threshold_state(slope_gate),
        "selector_read_cache_gate": threshold_state(selector_cache_gate),
        "selector_reference_width_gate": threshold_state(
            selector_width_gate),
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
          "Siblings/query | Child/query | Combined/query | Lookup us/query | "
          "Exact answers/query | Lookup us/(query+answer) | Back CPU % | "
          "Child hit % | Back MiB |")
    print("|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
    for row in rows:
        print("| {} | {} | {} | {} | {} | {} | {} | {} | {} | {} | {} | {} | {} |".format(
            fmt(row.get("user_cpu")), fmt(row.get("back_active")),
            fmt(row.get("back_groups_per_query"), 1),
            fmt(row.get("back_nodes_per_query"), 1),
            fmt(row.get("back_siblings_per_query"), 1),
            fmt(row.get("back_children_per_query"), 1),
            fmt(row.get("back_combined_per_query"), 1),
            fmt(row.get("back_lookup_us_per_query"), 1),
            fmt(row.get("back_exact_successes_per_query"), 3),
            fmt(row.get("back_lookup_us_per_answer_unit"), 1),
            fmt(row.get("back_lookup_cpu_pct"), 1),
            fmt(row.get("back_child_hit_pct"), 1),
            fmt(row.get("back_mib"), 1)))
    print()
    if any(number(row, "back_route_capacity", 0) > 0 for row in rows):
        print("| CPU s | Mask route % | Tree route % | Position route % | "
              "Mask cost/choice | Tree cost/choice | Position cost/choice | "
              "Switches | Reversions | Hysteresis holds |")
        print("|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
        for row in rows:
            print("| {} | {} | {} | {} | {} | {} | {} | {} | {} | {} |".format(
                fmt(row.get("user_cpu")),
                fmt(row.get("back_route_mask_choice_pct"), 1),
                fmt(row.get("back_route_tree_choice_pct"), 1),
                fmt(row.get("back_route_position_choice_pct"), 1),
                fmt(row.get("back_route_mask_cost_per_choice"), 1),
                fmt(row.get("back_route_tree_cost_per_choice"), 1),
                fmt(row.get("back_route_position_cost_per_choice"), 1),
                fmt(row.get("delta_back_route_switches")),
                fmt(row.get("delta_back_route_reversions")),
                fmt(row.get("delta_back_route_hysteresis_holds"))))
        print()
    if any(number(row, "back_edge_enabled", 0) > 0 or
           row.get("back_edge_enabled") == "yes" for row in rows):
        print("| CPU s | Edge features | Edge MiB | Edge queries | "
              "Empty | Bypassed | Posting records/query | "
              "Candidates/query | Exact reject % |")
        print("|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
        for row in rows:
            print("| {} | {} | {} | {} | {} | {} | {} | {} | {} |".format(
                fmt(row.get("user_cpu")),
                fmt(row.get("back_edge_features")),
                fmt(safe_ratio(row.get("back_edge_bytes"), 1024 * 1024), 1),
                fmt(row.get("delta_back_edge_queries")),
                fmt(row.get("delta_back_edge_empty_queries")),
                fmt(row.get("delta_back_edge_bypass_queries")),
                fmt(row.get("back_edge_posting_records_per_query"), 1),
                fmt(row.get("back_edge_candidates_per_query"), 1),
                fmt(row.get("back_edge_exact_reject_pct"), 1)))
        print()
    if any(number(row, "rewrite_subject_atoms", 0) > 0 for row in rows):
        print("| CPU s | Rewrite atoms | Initial nodes/atom | Attempts/atom | "
              "Target nodes/attempt | Legacy/new prep nodes |")
        print("|---:|---:|---:|---:|---:|---:|")
        for row in rows:
            print("| {} | {} | {} | {} | {} | {} |".format(
                fmt(row.get("user_cpu")),
                fmt(row.get("delta_rewrite_subject_atoms")),
                fmt(row.get("rewrite_initial_nodes_per_atom"), 1),
                fmt(row.get("rewrite_attempts_per_atom"), 1),
                fmt(row.get("rewrite_target_nodes_per_attempt"), 1),
                fmt(row.get("rewrite_query_preparation_reduction"), 1)))
        print()
    markdown_summary(summary)


def markdown_summary(summary):
    print("Final: strategy={}, {} givens, {} generated, {} kept, PSS={} MiB, "
          "back index={} MiB.".format(
              fmt(summary.get("strategy")), fmt(summary.get("last_given")),
              fmt(summary.get("last_generated")), fmt(summary.get("last_kept")),
              fmt(summary.get("last_pss_mib"), 1),
              fmt(summary.get("last_back_mib"), 1)))
    print("Peak memory={} MiB ({}).".format(
        fmt(summary.get("peak_rss_mib"), 1),
        fmt(summary.get("peak_rss_source"))))
    if summary.get("last_rewrite_query_preparation_reduction") is not None:
        print("Cumulative compact rewrite query-preparation node ratio: "
              "legacy/new={}x.".format(fmt(
                  summary.get("last_rewrite_query_preparation_reduction"),
                  1)))
    if number(summary, "last_back_route_profile_capacity", 0) > 0:
        print("Final adaptive route table={}/{} entries ({} bytes); last "
              "interval route mix: mask={}%, tree={}%, position={}%.".format(
                  fmt(summary.get("last_back_route_profile_occupied")),
                  fmt(summary.get("last_back_route_profile_capacity")),
                  fmt(summary.get("last_back_route_profile_bytes")),
                  fmt(summary.get("last_back_route_mask_choice_pct"), 1),
                  fmt(summary.get("last_back_route_tree_choice_pct"), 1),
                  fmt(summary.get("last_back_route_position_choice_pct"), 1)))
    print("First-to-last interval ratios: counted back work/query={}, "
          "back lookup CPU/query={}, answer-normalized back CPU={}, "
          "given/CPU={}.".format(
              fmt(summary.get("combined_slope_ratio"), 2),
              fmt(summary.get("back_lookup_cpu_slope_ratio"), 2),
              fmt(summary.get("back_lookup_normalized_cpu_slope_ratio"), 2),
              fmt(summary.get("given_rate_ratio"), 2)))
    print("Post-warm-up normalized CPU: early median={} us/unit, tail "
          "median={} us/unit, growth={}, tail spread={}, gate ratio={}.".format(
              fmt((summary.get(
                  "back_lookup_normalized_early_median_seconds") or 0) *
                  1000000.0, 1)
              if summary.get(
                  "back_lookup_normalized_early_median_seconds") is not None
              else "NA",
              fmt((summary.get(
                  "back_lookup_normalized_tail_median_seconds") or 0) *
                  1000000.0, 1)
              if summary.get(
                  "back_lookup_normalized_tail_median_seconds") is not None
              else "NA",
              fmt(summary.get(
                  "back_lookup_normalized_steady_growth_ratio"), 2),
              fmt(summary.get(
                  "back_lookup_normalized_tail_spread_ratio"), 2),
              fmt(summary.get(
                  "back_lookup_normalized_steady_slope_ratio"), 2)))
    if summary.get("last_selector_store") == "file":
        print("File-selector consumed-read cache advice: coverage={}%, "
              "failures={}; last interval min calls/given={}, run "
              "checks/given={}; record reference={} bits in {} bytes.".format(
                  fmt(summary.get("last_selector_read_eviction_pct"), 1),
                  fmt(summary.get(
                      "last_selector_read_eviction_failures")),
                  fmt(summary.get(
                      "last_selector_min_calls_per_given"), 2),
                  fmt(summary.get(
                      "last_selector_run_checks_per_given"), 2),
                  fmt(summary.get("last_selector_record_bits")),
                  fmt(summary.get("last_selector_entry_bytes"))))
    if summary["signals"]:
        print("Signals:")
        for signal in summary["signals"]:
            print("- " + signal)
    else:
        print("Signals: none in the parsed counters (mature acceptance still "
              "requires a matched reference run).")
    print()


def markdown_comparisons(comparisons):
    print("## Matched-run threshold audit")
    print()
    print("The first output is the reference. `eligible` means the parsed "
          "thresholds pass; it is not a substitute for independent proof "
          "checking or total-job/cgroup accounting.")
    if comparisons:
        first = comparisons[0]
        print("Thresholds: CPU ratio <= {}, RAM saving >= {}%, measured "
              "post-warm-up back-lookup CPU/(query + exact answer) gate "
              "ratio <= {}; at least {} complete intervals.".format(
                  fmt(first["max_cpu_ratio"], 2),
                  fmt(first["min_ram_saving_pct"], 1),
                  fmt(first["max_back_slope_ratio"], 2),
                  fmt(first["required_slope_samples"])))
    print()
    print("| Candidate | Trajectory | CPU ratio | CPU | Reference peak MiB | "
          "Candidate peak MiB | RAM saved % | RAM | Samples | Periodic | "
          "Normalized samples | Steady back CPU ratio | Slope | Read cache | "
          "Reference width | Result |")
    print("|:---|:---:|---:|:---:|---:|---:|---:|:---:|---:|:---:|---:|---:|:---:|:---:|:---:|:---:|")
    for comparison in comparisons:
        print("| {} | {} | {} | {} | {} | {} | {} | {} | {} | {} | {} | {} | {} | {} | {} | {} |".format(
            comparison["candidate"], comparison["trajectory_gate"],
            fmt(comparison["cpu_ratio"], 2), comparison["cpu_gate"],
            fmt(comparison["reference_peak_rss_mib"], 1),
            fmt(comparison["candidate_peak_rss_mib"], 1),
            fmt(comparison["ram_savings_pct"], 1), comparison["ram_gate"],
            fmt(comparison["candidate_periodic_samples"]),
            comparison["interval_gate"],
            fmt(comparison["candidate_normalized_samples"]),
            fmt(comparison["candidate_back_normalized_cpu_slope_ratio"], 2),
            comparison["slope_gate"],
            comparison["selector_read_cache_gate"],
            comparison["selector_reference_width_gate"],
            comparison["result"]))
    print()


TSV_COLUMNS = (
    "label", "sample", "user_cpu", "system_cpu", "wall_clock", "given",
    "generated", "kept", "delta_given", "delta_generated", "delta_kept",
    "given_per_cpu", "generated_per_given", "back_strategy", "back_active",
    "delta_back_queries", "back_groups_per_query",
    "back_mask_trie_nodes", "delta_back_mask_trie_queries",
    "back_mask_nodes_per_query", "delta_back_mask_trie_prunes",
    "back_mask_directory_blocks", "delta_back_mask_directory_queries",
    "delta_back_mask_directory_blocks_examined",
    "delta_back_mask_directory_word_checks",
    "delta_back_mask_directory_buckets_selected",
    "back_nodes_per_query",
    "back_siblings_per_query", "back_children_per_query",
    "back_combined_per_query", "back_lookup_cpu_pct", "back_child_hit_pct",
    "delta_back_tree_insert_sibling_checks",
    "delta_back_tree_insert_cache_lookups",
    "delta_back_tree_insert_cache_hits",
    "delta_back_tree_insert_cache_misses",
    "back_lookup_seconds_per_query", "back_lookup_us_per_query",
    "back_timing_lookup_timing", "back_timing_lookup_rate",
    "delta_back_timing_lookup_samples", "back_lookup_timing_sufficient",
    "delta_back_profile_exact_successes", "back_exact_successes_per_query",
    "back_lookup_seconds_per_answer_unit", "back_lookup_us_per_answer_unit",
    "back_route_capacity", "back_route_occupied", "back_route_bytes",
    "back_route_frequency_bytes", "delta_back_route_profile_hits",
    "delta_back_route_profile_misses", "delta_back_route_cold_fallbacks",
    "delta_back_route_admission_attempts",
    "delta_back_route_admission_rejections",
    "delta_back_route_aged_replacements",
    "delta_back_route_frequency_decays",
    "delta_back_route_pre_tree_observations",
    "delta_back_route_pre_tree_hot_observations",
    "delta_back_route_mask_choices", "delta_back_route_tree_choices",
    "delta_back_route_position_choices", "back_route_mask_choice_pct",
    "back_route_tree_choice_pct", "back_route_position_choice_pct",
    "back_route_mask_cost_per_choice", "back_route_tree_cost_per_choice",
    "back_route_position_cost_per_choice", "delta_back_route_mask_probes",
    "delta_back_route_tree_probes", "delta_back_route_position_probes",
    "delta_back_route_tree_probe_aborts",
    "delta_back_route_tree_probe_budget",
    "delta_back_route_tree_probe_discarded_candidates",
    "delta_back_route_switches", "delta_back_route_reversions",
    "delta_back_route_hysteresis_holds",
    "back_tree_child_parents", "back_tree_child_bytes", "back_bytes",
    "back_position_features", "back_position_active_roots",
    "delta_back_position_admissions",
    "delta_back_position_rejections", "delta_back_position_demotions",
    "delta_back_position_retry_deferrals",
    "delta_back_position_census_records",
    "delta_back_position_backfill_records",
    "delta_back_position_append_records",
    "delta_back_position_append_root_scans",
    "delta_back_position_append_token_visits",
    "delta_back_position_append_feature_lookups",
    "delta_back_position_append_matches",
    "back_position_credit_balance", "delta_back_position_credit_earned",
    "delta_back_position_credit_spent",
    "delta_back_position_credit_reservations",
    "delta_back_position_admission_freezes",
    "back_position_admission_frozen", "back_position_estimated",
    "back_edge_enabled", "back_edge_features", "back_edge_postings",
    "back_edge_bytes", "delta_back_edge_queries",
    "delta_back_edge_empty_queries", "delta_back_edge_bypass_queries",
    "delta_back_edge_intersection_queries",
    "delta_back_edge_query_features", "delta_back_edge_selected_features",
    "delta_back_edge_posting_records", "delta_back_edge_candidate_records",
    "delta_back_edge_exact_rejects",
    "back_edge_posting_records_per_query",
    "back_edge_candidates_per_query", "back_edge_exact_reject_pct",
    "delta_back_edge_append_records",
    "delta_back_edge_append_token_visits",
    "delta_back_edge_append_feature_lookups",
    "residency_pss", "residency_anonymous", "residency_swap",
    "passive_records", "passive_directory_logical", "selector_buffer_bytes",
    "selector_run_logical", "ancestor_file_reads_bytes",
    "ancestor_file_writes_bytes", "delta_ancestor_file_reads_bytes",
    "delta_ancestor_file_writes_bytes", "delta_selector_reads_bytes",
    "delta_selector_writes_bytes", "ancestor_io_mib_per_cpu",
    "selector_io_mib_per_cpu", "statistics_format_comma_num_buffers",
    "selector_read_evictions", "selector_read_evictions_bytes",
    "selector_read_eviction_failures", "selector_read_eviction_pct",
    "selector_record_bits", "selector_entry_bytes",
    "delta_selector_min_calls", "selector_min_calls_per_given",
    "delta_selector_buffer_checks", "delta_selector_run_checks",
    "selector_run_checks_per_given",
    "allocator_rss_current_kb", "allocator_rss_peak_kb",
    "external_peak_rss_kb", "external_user_cpu",
    "cgroup_memory_peak_bytes", "cgroup_memory_current_bytes",
    "cgroup_anon_current_bytes", "cgroup_file_current_bytes",
    "cgroup_shmem_current_bytes", "cgroup_swap_peak_bytes",
    "clock_infer", "clock_preprocess",
    "clock_demod", "clock_back_demod",
    "delta_rewrite_subject_atoms", "delta_rewrite_subject_initial_nodes",
    "delta_rewrite_subject_target_nodes", "delta_rewrite_subject_attempts",
    "rewrite_initial_nodes_per_atom", "rewrite_attempts_per_atom",
    "rewrite_target_nodes_per_attempt",
    "rewrite_query_preparation_reduction",
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
    parser.add_argument(
        "--compare-to-first", action="store_true",
        help="audit each later output against the first (Markdown/JSON only)")
    parser.add_argument("--max-cpu-ratio", type=float, default=1.25,
                        help="comparison CPU rejection threshold (default 1.25)")
    parser.add_argument(
        "--min-ram-saving-pct", type=float, default=80.0,
        help="comparison peak-RSS saving threshold (default 80)")
    parser.add_argument(
        "--max-back-slope-ratio", type=float, default=1.25,
        help=("comparison post-warm-up back-lookup CPU/(query + exact answer) "
              "growth/tail-spread threshold (default 1.25)"))
    args = parser.parse_args(argv)
    if args.tail < 0:
        parser.error("--tail must be nonnegative")
    if args.compare_to_first and len(args.outputs) < 2:
        parser.error("--compare-to-first requires at least two outputs")
    if args.compare_to_first and args.format == "tsv":
        parser.error("--compare-to-first is available in Markdown or JSON")
    if args.max_cpu_ratio <= 0 or args.max_back_slope_ratio <= 0:
        parser.error("CPU and back-slope ratios must be positive")
    if not 0 <= args.min_ram_saving_pct <= 100:
        parser.error("--min-ram-saving-pct must be between 0 and 100")
    runs = []
    basenames = ["stdin" if path == "-" else os.path.basename(path)
                 for path in args.outputs]
    for path, basename in zip(args.outputs, basenames):
        label = (os.path.relpath(path) if path != "-" and
                 basenames.count(basename) > 1 else basename)
        rows = derive_intervals(parse_run(path))
        runs.append((label, rows, run_summary(label, rows)))
    comparisons = ([compare_summaries(
                        runs[0][2], run[2], args.max_cpu_ratio,
                        args.min_ram_saving_pct, args.max_back_slope_ratio)
                    for run in runs[1:]] if args.compare_to_first else [])
    if args.format == "tsv":
        emit_tsv(runs)
    elif args.format == "json":
        print(json.dumps({"runs": [
            {"label": label, "samples": rows, "summary": summary}
            for label, rows, summary in runs],
            "comparisons": comparisons}, indent=2, sort_keys=True))
    else:
        for label, rows, summary in runs:
            if args.summary_only:
                print("## " + label)
                print()
                markdown_summary(summary)
            else:
                shown = rows if args.tail == 0 else rows[-args.tail:]
                markdown(label, shown, summary, len(rows))
        if comparisons:
            markdown_comparisons(comparisons)
    return 0


if __name__ == "__main__":
    sys.exit(main())
