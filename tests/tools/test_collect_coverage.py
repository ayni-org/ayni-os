"""Coverage aggregation normal inputs: same layout unions, distinct layouts stay separate."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("coverage_tool", ROOT / "tools/collect_coverage.py")
TOOL = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(TOOL)

class CoverageAggregation(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.coverage = TOOL.Coverage(self.root)

    def add(self, lines, source="core/example.h"):
        self.coverage.add({
            "format_version": "1", "gcc_version": "13.3.0",
            "current_working_directory": str(self.root),
            "files": [{"file": source, "lines": lines}],
        })

    @staticmethod
    def line(count, arcs, function="same"):
        return {"line_number": 10, "count": count, "function_name": function,
                "branches": [{"count": hit, "fallthrough": flag} for hit, flag in arcs]}

    def test_same_function_and_layout_union_across_translation_units(self):
        self.add([self.line(0, [(0, True), (1, False)])])
        self.add([self.line(2, [(1, True), (0, False)])])
        self.assertEqual(self.coverage.measure(("core/",)),
                         {"lines": [1, 1], "branches": [2, 2]})

    def test_different_layout_or_template_instance_keeps_separate_slots(self):
        self.add([self.line(1, [(1, True), (1, False)])])
        self.add([self.line(1, [(0, True), (1, False), (0, False)])])
        self.add([self.line(1, [(0, True), (1, False)], "other")])
        self.assertEqual(self.coverage.measure(("core/",))["branches"], [4, 7])

    def test_only_throw_arcs_are_excluded_and_zero_hit_defensive_arcs_remain(self):
        line = self.line(0, [(0, True), (0, False)])
        line["branches"].append({"count": 1, "fallthrough": False, "throw": True})
        self.add([line])
        self.assertEqual(self.coverage.measure(("core/",)),
                         {"lines": [0, 1], "branches": [0, 2]})

    def test_paths_normalize_and_sources_outside_root_do_not_enter_scope(self):
        self.add([self.line(1, [])], "core/../core/example.h")
        self.add([self.line(1, [])], "../external/core/example.h")
        self.assertEqual(self.coverage.seen, {"core/example.h"})
        (self.root / "core").mkdir()
        (self.root / "core/unrepresented.cpp").touch()
        self.assertEqual(self.coverage.missing("*.cpp"), ["core/unrepresented.cpp"])

    def test_compilation_database_ignores_stale_notes_and_reports_missing_units(self):
        build = self.root / "build"
        build.mkdir()
        (build / "current.gcno").touch()
        (build / "old.gcno").touch()
        units = [{"directory": str(build), "output": name + ".o", "file": name + ".cpp",
                  "command": "c++ --coverage -fprofile-update=atomic -c " + name + ".cpp"}
                 for name in ("current", "absent")]
        (build / "compile_commands.json").write_text(json.dumps(units), encoding="utf-8")
        notes, missing, stale = TOOL.current_notes(build)
        self.assertEqual(notes, [build / "current.gcno"])
        self.assertEqual(missing, ["absent.cpp"])
        self.assertEqual(stale, 1)

    def test_noncoverage_build_cannot_reuse_an_old_note(self):
        build = self.root / "build"
        build.mkdir()
        (build / "old.gcno").touch()
        units = [{"directory": str(build), "output": "old.o", "file": "old.cpp",
                  "command": "c++ -c old.cpp"}]
        (build / "compile_commands.json").write_text(json.dumps(units), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "atomic counters"):
            TOOL.current_notes(build)

if __name__ == "__main__":
    unittest.main()
