#!/usr/bin/env python3

import gzip
import importlib.util
import io
import os
import tempfile
import unittest
from contextlib import redirect_stdout


HERE = os.path.dirname(os.path.abspath(__file__))
SPEC = importlib.util.spec_from_file_location(
    "compact_long_run_report", os.path.join(HERE, "compact_long_run_report.py"))
REPORT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(REPORT)


SAMPLE = """\
Statistics_format: comma_num_buffers=32.
Given=100. Generated=1000. Kept=200. proofs=0.
Usable=10. Sos=20. Demods=3. Limbo=0, Disabled=4. Hints=5. Active_Hints=2.
Compact_back_demod: mode=authoritative, strategy=adaptive, failures=0, active=50, queries=20, candidates=4, groups_examined=200, tree_nodes_examined=100, tree_sibling_checks=50, tree_child_lookups=10, tree_child_hits=8, tree_child_parents=2, tree_child_bytes=1024, bytes=4096.
Compact_index_timing: component=back_demod, lookup_seconds=2.0, exact_seconds=0.1, materialize_seconds=0.2, maintenance_seconds=0.3.
Dense_passive: backing=ancestor-file, directory=file, records=20, directory_logical=1280.
Dense_passive_selector: store=file, buffer_bytes=240, run_logical=480, reads=3 (72 bytes), writes=4 (96 bytes).
Dense_passive_gc: validation_failures=0.
Ancestor_store: validation_failures=0, file_reads=10 (1000 bytes), file_writes=5 (500 bytes).
Process_residency_kb: pss=10240, anonymous=8192, swap=0.
Allocator_slabs: current=1, peak=2, RSS_kb: current=10240, peak=11000.
User_CPU=10.00, System_CPU=1.00, Wall_clock=11.
clock infer          :   1.00 seconds.
clock preprocess     :   8.00 seconds.
clock demod          :   3.00 seconds.
clock back_demod     :   2.20 seconds.
Given=140. Generated=1800. Kept=300. proofs=0.
Compact_back_demod: mode=authoritative, strategy=adaptive, failures=0, active=90, queries=30, candidates=6, groups_examined=350, tree_nodes_examined=180, tree_sibling_checks=70, tree_child_lookups=20, tree_child_hits=17, tree_child_parents=3, tree_child_bytes=2048, bytes=8192.
Compact_index_timing: component=back_demod, lookup_seconds=3.0, exact_seconds=0.2, materialize_seconds=0.3, maintenance_seconds=0.4.
Process_residency_kb: pss=12288, anonymous=9216, swap=1024.
Allocator_slabs: current=2, peak=3, RSS_kb: current=12288, peak=13000.
User_CPU=20.00, System_CPU=2.00, Wall_clock=22.
clock infer          :   2.00 seconds.
clock preprocess     :  17.00 seconds.
clock demod          :   7.00 seconds.
clock back_demod     :   3.40 seconds.
"""


class ReportTest(unittest.TestCase):
    def test_periodic_blocks_include_clocks_after_user_cpu(self):
        rows = REPORT.derive_intervals(REPORT.parse_stream(
            SAMPLE.splitlines(True), "synthetic"))
        self.assertEqual(len(rows), 2)
        self.assertEqual(rows[0]["clock_preprocess"], 8.0)
        self.assertEqual(rows[1]["delta_clock_preprocess"], 9.0)
        self.assertAlmostEqual(rows[1]["given_per_cpu"], 4.0)
        self.assertAlmostEqual(rows[1]["generated_per_given"], 20.0)
        self.assertAlmostEqual(rows[1]["back_groups_per_query"], 15.0)
        self.assertAlmostEqual(rows[1]["back_nodes_per_query"], 8.0)
        self.assertAlmostEqual(rows[1]["back_siblings_per_query"], 2.0)
        self.assertAlmostEqual(rows[1]["back_children_per_query"], 1.0)
        self.assertAlmostEqual(rows[1]["back_combined_per_query"], 26.0)
        self.assertAlmostEqual(rows[1]["back_lookup_cpu_pct"], 10.0)
        self.assertAlmostEqual(rows[1]["back_child_hit_pct"], 85.0)
        self.assertAlmostEqual(rows[1]["clock_preprocess_cpu_pct"], 90.0)
        self.assertAlmostEqual(rows[1]["clock_demod_cpu_pct"], 40.0)
        self.assertEqual(rows[0]["ancestor_file_reads_bytes"], 1000)
        self.assertEqual(rows[0]["selector_writes_bytes"], 96)
        self.assertAlmostEqual(rows[1]["pss_mib"], 12.0)
        self.assertEqual(rows[1]["statistics_format_comma_num_buffers"], 32)
        self.assertAlmostEqual(rows[0]["ancestor_io_mib_per_cpu"],
                               1500 / (10 * 1024 * 1024))

    def test_gzip_and_summary_signals(self):
        descriptor, path = tempfile.mkstemp(suffix=".out.gz")
        os.close(descriptor)
        try:
            with gzip.open(path, "wt") as stream:
                stream.write(SAMPLE)
            rows = REPORT.derive_intervals(REPORT.parse_file(path))
            summary = REPORT.run_summary("gzip", rows)
            self.assertEqual(summary["last_given"], 140)
            self.assertEqual(summary["strategy"], "adaptive")
            self.assertIn("process reports nonzero swap residency",
                          summary["signals"])
            self.assertEqual(summary["statistics_format_buffers"], 32)
            self.assertEqual(summary["peak_rss_kb"], 13000)
            self.assertEqual(summary["peak_rss_source"],
                             "Prover9 allocator report")
            self.assertNotIn(
                "output predates the safe long-statistics formatting marker; "
                "long comma-formatted lines may contain overwritten fields",
                summary["signals"])
        finally:
            os.unlink(path)

    def test_legacy_output_without_compact_statistics(self):
        text = """Given=2. Generated=10. Kept=4. proofs=0.\nUser_CPU=1.0, System_CPU=0.0, Wall_clock=1.\n"""
        rows = REPORT.derive_intervals(REPORT.parse_stream(text.splitlines(True)))
        self.assertEqual(rows[0]["given_per_cpu"], 2.0)
        self.assertIsNone(rows[0]["back_combined_per_query"])

    def test_missing_timing_and_counter_reset_are_safe_and_visible(self):
        text = """\
Given=10. Generated=100. Kept=20. proofs=0.
Compact_back_demod: queries=5, groups_examined=50.
User_CPU=10.0, System_CPU=0.0, Wall_clock=10.
Given=3. Generated=30. Kept=4. proofs=0.
Compact_back_demod: queries=2, groups_examined=20.
User_CPU=2.0, System_CPU=0.0, Wall_clock=2.
"""
        rows = REPORT.derive_intervals(REPORT.parse_stream(text.splitlines(True)))
        self.assertIsNone(rows[1]["back_lookup_cpu_pct"])
        self.assertTrue(rows[1]["counter_reset"])
        self.assertAlmostEqual(rows[1]["given_per_cpu"], 1.5)
        summary = REPORT.run_summary("restart", rows)
        self.assertIn("one or more cumulative counters reset between reports",
                      summary["signals"])
        self.assertIn("output predates the safe long-statistics formatting marker; "
                      "long comma-formatted lines may contain overwritten fields",
                      summary["signals"])

    def test_markdown_tail_keeps_full_run_summary(self):
        rows = REPORT.derive_intervals(REPORT.parse_stream(
            SAMPLE.splitlines(True), "synthetic"))
        summary = REPORT.run_summary("synthetic", rows)
        output = io.StringIO()
        with redirect_stdout(output):
            REPORT.markdown("synthetic", rows[-1:], summary, len(rows))
        rendered = output.getvalue()
        self.assertIn("Showing the last 1 of 2 periodic intervals.", rendered)
        self.assertIn("First-to-last interval ratios", rendered)
        self.assertIn("Preprocess CPU %", rendered)

    def test_markdown_summary_only_omits_interval_tables(self):
        rows = REPORT.derive_intervals(REPORT.parse_stream(
            SAMPLE.splitlines(True), "synthetic"))
        summary = REPORT.run_summary("synthetic", rows)
        output = io.StringIO()
        with redirect_stdout(output):
            REPORT.markdown_summary(summary)
        self.assertIn("Final: strategy=adaptive", output.getvalue())
        self.assertNotIn("Given/CPU", output.getvalue())

    def test_gnu_time_sidecar_overrides_allocator_peak(self):
        with tempfile.TemporaryDirectory() as directory:
            output_path = os.path.join(directory, "candidate.out")
            time_path = os.path.join(directory, "candidate.time")
            with open(output_path, "w") as stream:
                stream.write(SAMPLE)
            with open(time_path, "w") as stream:
                stream.write("\tUser time (seconds): 21.25\n")
                stream.write("\tMaximum resident set size (kbytes): 54321\n")
            rows = REPORT.derive_intervals(REPORT.parse_run(output_path))
            summary = REPORT.run_summary("candidate", rows)
            self.assertEqual(summary["last_user_cpu"], 20.0)
            self.assertEqual(summary["peak_rss_kb"], 54321)
            self.assertEqual(summary["peak_rss_source"], "GNU time sidecar")

    def test_matched_threshold_audit(self):
        reference = {
            "label": "old", "last_given": 100, "last_generated": 1000,
            "last_kept": 200, "last_proofs": 0, "last_user_cpu": 100.0,
            "peak_rss_kb": 100000, "peak_rss_mib": 100000 / 1024,
            "samples": 2, "combined_slope_ratio": 1.0,
        }
        candidate = {
            "label": "new", "last_given": 100, "last_generated": 1000,
            "last_kept": 200, "last_proofs": 0, "last_user_cpu": 120.0,
            "peak_rss_kb": 19000, "peak_rss_mib": 19000 / 1024,
            "samples": 3, "combined_slope_ratio": 1.20,
        }
        comparison = REPORT.compare_summaries(reference, candidate)
        self.assertEqual(comparison["result"], "eligible")
        self.assertEqual(comparison["trajectory_gate"], "pass")
        self.assertEqual(comparison["cpu_gate"], "pass")
        self.assertEqual(comparison["ram_gate"], "pass")
        self.assertEqual(comparison["slope_gate"], "pass")

        candidate["peak_rss_kb"] = 100000
        candidate["peak_rss_mib"] = 100000 / 1024
        custom = REPORT.compare_summaries(
            reference, candidate, min_ram_saving_pct=0.0)
        self.assertEqual(custom["ram_gate"], "pass")
        self.assertEqual(custom["result"], "eligible")
        candidate["peak_rss_kb"] = 19000
        candidate["peak_rss_mib"] = 19000 / 1024

        candidate["last_user_cpu"] = 130.0
        self.assertEqual(REPORT.compare_summaries(
            reference, candidate)["result"], "reject")
        candidate["last_user_cpu"] = 120.0
        candidate["peak_rss_kb"] = None
        candidate["peak_rss_mib"] = None
        self.assertEqual(REPORT.compare_summaries(
            reference, candidate)["result"], "incomplete")

    def test_comparison_markdown_names_thresholds(self):
        comparison = {
            "candidate": "new", "trajectory_gate": "pass",
            "cpu_ratio": 1.1, "cpu_gate": "pass",
            "reference_peak_rss_mib": 100.0,
            "candidate_peak_rss_mib": 10.0, "ram_savings_pct": 90.0,
            "ram_gate": "pass", "candidate_periodic_samples": 3,
            "interval_gate": "pass",
            "candidate_back_slope_ratio": 1.1, "slope_gate": "pass",
            "max_cpu_ratio": 1.25, "min_ram_saving_pct": 80.0,
            "max_back_slope_ratio": 1.25,
            "result": "eligible",
        }
        output = io.StringIO()
        with redirect_stdout(output):
            REPORT.markdown_comparisons([comparison])
        self.assertIn("Matched-run threshold audit", output.getvalue())
        self.assertIn("| new | pass | 1.10 | pass |", output.getvalue())


if __name__ == "__main__":
    unittest.main()
