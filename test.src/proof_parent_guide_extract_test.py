#!/usr/bin/env python3
"""Unit tests for utilities/extract_proof_parent_guide.py."""

import importlib.util
import io
import os
import sys
import tempfile
import unittest


HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(HERE, "..", "utilities", "extract_proof_parent_guide.py")
SPEC = importlib.util.spec_from_file_location("proof_parent_guide_extract", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


SAMPLE = """\
============================== PROOF =================================
1 a = a.  [assumption].
2 b = b.  [assumption].
3 a = b.  [para(1(a,1),2(a,1,1)),rewrite([1(2)])].
4 b = a.  [hyper(3,a,2,a,b),rewrite([1(1),2(2)])].
============================== end of proof ==========================
"""


class ExtractProofParentGuideTest(unittest.TestCase):
    def records(self):
        with tempfile.NamedTemporaryFile("w", encoding="utf-8", delete=False) as fp:
            fp.write(SAMPLE)
            path = fp.name
        try:
            return MODULE.read_records(path)
        finally:
            os.unlink(path)

    def test_extracts_primary_and_rewrite_parents(self):
        records = self.records()
        self.assertEqual([record.rule for record in records],
                         ["assumption", "assumption", "para", "hyper"])
        self.assertEqual(records[2].parents, (1, 2))
        self.assertEqual(records[2].rewrite_parents, (1,))
        self.assertEqual(records[3].parents, (3, 2))
        self.assertEqual(records[3].rewrite_parents, (1, 2))

    def test_back_rewrite_is_not_mistaken_for_rewrite_list(self):
        record = MODULE.parse_record(
            "9 c = c.  [back_rewrite(7),flip(a)].")
        self.assertEqual(record.rule, "back_rewrite")
        self.assertEqual(record.parents, (7,))
        self.assertEqual(record.rewrite_parents, ())

    def test_emits_auxiliary_formula_list_and_labels(self):
        output = io.StringIO()
        MODULE.write_guide(self.records(), output, "sample.out")
        text = output.getvalue()
        self.assertIn("formulas(proof_parent_guide).", text)
        self.assertIn('label("proof_parent_node=3")', text)
        self.assertIn('label("proof_parent_para=1,2")', text)
        self.assertIn('label("proof_parent_rewrite=1")', text)
        self.assertTrue(text.endswith("end_of_list.\n"))

    def test_rejects_forward_reference(self):
        broken = SAMPLE.replace("para(1(a,1),2(a,1,1))",
                                "para(4(a,1),2(a,1,1))")
        with tempfile.NamedTemporaryFile("w", encoding="utf-8", delete=False) as fp:
            fp.write(broken)
            path = fp.name
        try:
            with self.assertRaisesRegex(MODULE.GuideError, "non-prior parent 4"):
                MODULE.read_records(path)
        finally:
            os.unlink(path)


if __name__ == "__main__":
    unittest.main()
