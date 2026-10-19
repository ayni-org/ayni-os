"""Scenarios for tools/check_contracts.py, each on a throwaway repository tree."""

import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import check_contracts  # noqa: E402

README = """# Contracts

| 前缀 | 文档 | 属主 |
|---|---|---|
| `C-AAA`、`C-BBB` | [domain/a.md](domain/a.md) | `kernel` |
| `C-CCC` | [domain/c.md](domain/c.md) | `work` |
"""

PLAN = """# Plan

### 10.2 E1：one
| 步骤 | 预期 | 断言 |
|---|---|---|
| 1. first | x | y |
| 2. second | x | y |

### 10.3 E2：two
| 注入点 | 预期 | 断言 |
|---|---|---|
| F0：zero | x | y |
| F1：one | x | y |

### 10.4 E3：three
| 1. only | x | y |

### 10.5 E4：four
| 1. only | x | y |

### 10.6 not an experiment
| 1. ignored | x | y |
"""

HEADER = "# A\n\n- **关联**：计划 §2.8、§3.4；ADR-0002\n\n| ID | 规则 | 反例 | 验证 |\n|---|---|---|---|\n"
COVERING_ROW = "| C-AAA-001 | rule | counter | E1 步骤 1、2；E2 F0–F1；E3 步骤 1；E4 步骤 1；`MA-010` |\n"


class CheckContractsTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self.write("contracts/README.md", README)
        self.write("docs/ayni_v1.0.md", PLAN)
        self.write("contracts/domain/a.md", HEADER + COVERING_ROW)
        self.write("tests/unit/sample_test.cpp", "TEST(Sample, works) {}\nTEST_F(Fixture, also_works) {}\n")
        self.write("tests/CMakeLists.txt", "add_test(NAME sample_ctest COMMAND true)\n")

    def tearDown(self):
        self._tmp.cleanup()

    def write(self, relative, text):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def add_rows(self, rows, doc="contracts/domain/a.md"):
        path = self.root / doc
        existing = path.read_text(encoding="utf-8") if path.exists() else HEADER
        path.write_text(existing + rows, encoding="utf-8")

    def problems(self):
        check_contracts.check(self.root, write=True)
        return check_contracts.check(self.root)

    def assert_single_problem(self, fragment):
        found = self.problems()
        self.assertEqual(len(found), 1, found)
        self.assertIn(fragment, found[0])

    def test_clean_tree_passes_after_generation(self):
        self.assertEqual(self.problems(), [])
        matrix = (self.root / "contracts/traceability.md").read_text(encoding="utf-8")
        self.assertIn("| E2 F1 | C-AAA-001 |", matrix)
        self.assertIn("| §2.8 | domain/a.md |", matrix)
        self.assertIn("| `MA-010` | C-AAA-001 |", matrix)

    def test_duplicate_id_is_rejected(self):
        self.add_rows("| C-AAA-001 | again | counter | review |\n")
        self.assert_single_problem("C-AAA-001 is already defined at contracts/domain/a.md:7")

    def test_row_with_missing_column_is_rejected(self):
        self.add_rows("| C-AAA-002 | rule | review |\n")
        self.assert_single_problem("C-AAA-002 has 3 cells, expected 4")

    def test_pipe_inside_a_cell_is_rejected(self):
        self.add_rows("| C-AAA-002 | a | b | rule | review |\n")
        self.assert_single_problem("C-AAA-002 has 5 cells, expected 4")

    def test_empty_cell_is_rejected(self):
        self.add_rows("| C-AAA-002 | rule |  | review |\n")
        self.assert_single_problem("C-AAA-002 has an empty cell")

    def test_clause_in_another_prefix_document_is_rejected(self):
        self.add_rows("| C-CCC-001 | rule | counter | review |\n")
        self.assert_single_problem("C-CCC-001 belongs in contracts/domain/c.md")

    def test_unknown_prefix_is_rejected(self):
        self.add_rows("| C-ZZZ-001 | rule | counter | review |\n")
        self.assert_single_problem("prefix C-ZZZ is not in the contracts/README.md prefix table")

    def test_second_prefix_of_a_row_maps_to_the_same_document(self):
        self.add_rows("| C-BBB-001 | rule | counter | review |\n")
        self.assertEqual(self.problems(), [])

    def test_clauses_need_a_related_line(self):
        self.write("contracts/domain/c.md", "# C\n\n| C-CCC-001 | rule | counter | review |\n")
        self.assert_single_problem("contracts/domain/c.md: has clauses but no '- **关联**：' line")

    def test_undefined_reference_in_a_header_is_rejected(self):
        self.write("include/ayni/draft/x.h", "/* see C-AAA-001 and C-AAA-099 */\n")
        self.assert_single_problem("include/ayni/draft/x.h:1: C-AAA-099 is not defined")

    def test_references_in_tests_are_not_checked(self):
        self.write("tests/tools/fixture.md", "C-QQQ-001\n")
        self.assertEqual(self.problems(), [])

    def test_cited_tests_must_exist(self):
        self.add_rows("| C-AAA-002 | rule | counter | M0 已实现：`Sample.works`、`Fixture.also_works`、ctest `sample_ctest` |\n")
        self.assertEqual(self.problems(), [])
        self.add_rows("| C-AAA-003 | rule | counter | `Sample.missing` |\n")
        self.assert_single_problem("cites test Sample.missing, which is not defined under tests/")

    def test_cited_ctest_must_exist(self):
        self.add_rows("| C-AAA-002 | rule | counter | ctest `nothing_here` |\n")
        self.assert_single_problem("cites ctest nothing_here")

    def test_uncovered_experiment_step_is_rejected(self):
        self.write("contracts/domain/a.md", HEADER + "| C-AAA-001 | rule | counter | E1 步骤 1；E2 F0–F1；E3 步骤 1；E4 步骤 1 |\n")
        self.assert_single_problem("experiment step 'E1 步骤 2' is not named")

    def test_step_lists_and_ranges_expand(self):
        self.assertEqual(check_contracts.cited_steps("E1 步骤 1、3–4（备注 10 次）"), ["E1 步骤 1", "E1 步骤 3", "E1 步骤 4"])
        self.assertEqual(check_contracts.cited_steps("E2 F3–F5；E2 F7、F10"), ["E2 F3", "E2 F4", "E2 F5", "E2 F7", "E2 F10"])

    def test_stale_traceability_is_rejected(self):
        check_contracts.check(self.root, write=True)
        self.add_rows("| C-AAA-002 | rule | counter | review |\n")
        found = check_contracts.check(self.root)
        self.assertEqual(found, ["contracts/traceability.md: out of date; run tools/check_contracts.py --write"])

    def test_implemented_clauses_are_listed(self):
        self.add_rows("| C-AAA-002 | rule | counter | M0 已实现：`Sample.works` |\n")
        self.problems()
        matrix = (self.root / "contracts/traceability.md").read_text(encoding="utf-8")
        self.assertIn("| C-AAA-002 | M0 已实现：`Sample.works` |", matrix)
        self.assertIn("| 合计 | 2 | 1 |", matrix)

    def test_missing_readme_is_an_input_error(self):
        (self.root / "contracts/README.md").unlink()
        self.assertEqual(check_contracts.main(["--root", str(self.root)]), 2)


if __name__ == "__main__":
    unittest.main()
