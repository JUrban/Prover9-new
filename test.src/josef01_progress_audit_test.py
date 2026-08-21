#!/usr/bin/env python3

import importlib.util
import os
import unittest


HERE = os.path.dirname(os.path.abspath(__file__))
SPEC = importlib.util.spec_from_file_location(
    "josef01_progress_audit",
    os.path.join(HERE, "josef01_progress_audit.py"))
AUDIT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(AUDIT)


def report_block(index, terminal=False):
    given = 30827 if terminal else index * 100
    generated = 1602769536 if terminal else index * 100000
    kept = 36195388 if terminal else index * 10000
    proofs = 1 if terminal else 0
    matched = 48968 if terminal else index * 100
    return """\
Statistics_format: comma_num_buffers=32.
Given={given}. Generated={generated}. Kept={kept}. proofs={proofs}.
Generated_by_rule: binary=0, hyper={hyper}, ur=0, paramod={paramod}, other=54.
Search_loop: mode=otter, frontier=clauses, active_indexed=1.
Compact_unit_index: mode=authoritative, strategy=adaptive, feature_depth=2, failures=0, conflict_queries={conflicts}, conflict_exact_tests={exact}, position_queries={position}, position_postings={postings}, code_tree_queries={tree_queries}, code_tree_nodes={tree_nodes}, position_refinement_checks={refine_checks}, position_refinement_rejects={refine_rejects}, position_tertiary_checks={tertiary_checks}, position_tertiary_rejects={tertiary_rejects}, features={features}, bytes={unit_bytes}.
Hint_index: mode=packed_fast, fpa_depth=0.
Dense_passive: backing=ancestor-file, directory=file, directory_entry_bytes=64, records={kept}.
Dense_passive_selector: store=file, buffer_limit=1048576, record_bits=64, entry_bytes=16, flushes={flushes}, merges={merges}, reads={reads} ({read_bytes} bytes), writes={writes} ({write_bytes} bytes).
Packed_fast_conjunction: enabled=yes, queries={cache_queries}, posting_candidates={candidates}, profile_rejects={profile_rejects}, summary_reject_queries={summary_queries}, summary_reject_candidates={summary_rejects}.
Packed_fast_cache: budget_bytes=2097152, entries=16384, queries={cache_queries}, hits={hits}, misses={misses}, stores={stores}, min_candidates=128, admission_skips={skips}, arena_wraps={wraps}, overlap_invalidations=0, arena_expired_misses={expired}, posting_candidates_avoided={avoided}.
Process_residency_kb: pss={pss}, anonymous={anonymous}, swap=0.
User_CPU={user:.2f}, System_CPU={system:.2f}, Wall_clock={wall}.
Periodic_report_poll: generated={generated}, cpu_time_reads={reads_cpu}, interval=256.
Hint match stats:
  total=153681, redundant=57003, active=96678, matched={matched}
""".format(
        given=given, generated=generated, kept=kept, proofs=proofs,
        hyper=index * 1000, paramod=index * 99000,
        conflicts=index * 10000, exact=index * 500,
        position=index * 8000, postings=index * 20000,
        tree_queries=index * 2000, tree_nodes=index * 4000,
        refine_checks=index * 10000, refine_rejects=index * 8000,
        tertiary_checks=index * 4000, tertiary_rejects=index * 3000,
        features=index * 1048576, unit_bytes=index * 2097152,
        flushes=index * 2, merges=index, reads=index * 3,
        read_bytes=index * 1048576, writes=index * 4,
        write_bytes=index * 2097152, cache_queries=index * 10000,
        candidates=index * 100000, profile_rejects=index * 80000,
        summary_queries=index * 1000, summary_rejects=index * 10000,
        hits=index * 1000, misses=index * 9000, stores=index * 500,
        skips=index * 8500, wraps=index * 2, expired=index * 10,
        avoided=index * 200000, pss=500000 + index * 1000,
        anonymous=450000 + index * 1000, user=index * 100.0,
        system=index * 10.0, wall=index * 110,
        reads_cpu=(generated // 256), matched=matched)


class JosefProgressAuditTest(unittest.TestCase):
    def test_interval_metrics_and_authority_configuration(self):
        text = "".join(report_block(index) for index in range(1, 8))
        samples = AUDIT.long_report.parse_stream(text.splitlines(True))
        rows = AUDIT.derive_intervals(samples)
        self.assertEqual(len(rows), 7)
        self.assertAlmostEqual(rows[-1]["given_per_total_cpu"], 100 / 110)
        self.assertAlmostEqual(rows[-1]["generated_per_total_cpu"],
                               100000 / 110)
        self.assertAlmostEqual(rows[-1]["unit_tree_route_pct"], 20.0)
        self.assertAlmostEqual(rows[-1]["unit_position_route_pct"], 80.0)
        self.assertAlmostEqual(rows[-1]["unit_refinement_reject_pct"], 80.0)
        self.assertAlmostEqual(rows[-1]["unit_tertiary_reject_pct"], 75.0)
        self.assertAlmostEqual(rows[-1]["hint_cache_hit_pct"], 10.0)
        self.assertAlmostEqual(rows[-1]["hint_summary_reject_pct"], 10.0)
        self.assertEqual(rows[-1]["delta_selector_flushes"], 2)
        summary = AUDIT.summarize("synthetic", rows)
        self.assertEqual(summary["endpoint_state"], "in_progress")
        self.assertAlmostEqual(summary["throughput_slope_ratio"], 1.0)
        self.assertEqual(summary["signals"], [])

    def test_exact_terminal_endpoint(self):
        samples = AUDIT.long_report.parse_stream(
            report_block(1, terminal=True).splitlines(True))
        summary = AUDIT.summarize(
            "terminal", AUDIT.derive_intervals(samples))
        self.assertEqual(summary["endpoint_state"], "exact_proof_endpoint")
        self.assertNotIn(
            "terminal Josef trajectory does not match the authority endpoint "
            "and hint total", summary["signals"])

    def test_bad_mature_configuration_and_swap_are_visible(self):
        text = report_block(1).replace(
            "strategy=adaptive, feature_depth=2",
            "strategy=code_tree, feature_depth=0").replace(
                "entry_bytes=16", "entry_bytes=24").replace(
                    "min_candidates=128", "min_candidates=0").replace(
                        "swap=0", "swap=1024")
        samples = AUDIT.long_report.parse_stream(text.splitlines(True))
        summary = AUDIT.summarize("bad", AUDIT.derive_intervals(samples))
        joined = "\n".join(summary["signals"])
        self.assertIn("process reports nonzero swap residency", joined)
        self.assertIn("compact unit strategy", joined)
        self.assertIn("compact unit feature depth", joined)
        self.assertIn("selector entry width", joined)
        self.assertIn("hint cache admission threshold", joined)

    def test_incomplete_live_tail_is_ignored(self):
        text = (report_block(1) +
                "============================== STATISTICS " + "=" * 20 + "\n" +
                "Given=200. Generated=200000. Kept=20000. proofs=0.\n")
        samples = AUDIT.long_report.parse_stream(text.splitlines(True))
        self.assertEqual(len(samples), 2)
        complete = AUDIT.complete_samples(samples)
        self.assertEqual(len(complete), 1)
        self.assertEqual(complete[0]["given"], 100)


if __name__ == "__main__":
    unittest.main()
