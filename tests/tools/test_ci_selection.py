"""Verify the user-approved PR/periodic validation policy on normal changes."""
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[2] / "tools/ci/select_checks.py"
spec = importlib.util.spec_from_file_location("ci_selection", SCRIPT)
selection = importlib.util.module_from_spec(spec)
spec.loader.exec_module(selection)


class CiSelectionTest(unittest.TestCase):
    def enabled(self, paths, event="pull_request", full=False):
        return {name for name, enabled in selection.select_checks(event, paths, full).items()
                if enabled}

    def test_ordinary_core_change_runs_only_asan(self):
        self.assertEqual(self.enabled(["core/work/task/task_table.cpp"]), {"asan"})

    def test_markdown_and_main_push_do_not_build(self):
        self.assertEqual(self.enabled(["AGENTS.md", "docs/workflow.md"]), set())
        self.assertEqual(self.enabled(["core/kernel/ingress_lock.h"], "push"), set())

    def test_concurrency_and_esp_changes_use_only_the_daily_suite(self):
        self.assertEqual(self.enabled(["core/kernel/ports/completion_pool.cpp"]),
                         {"asan"})
        self.assertEqual(self.enabled(["hosts/esp_idf/esp_idf_host.cpp"]),
                         {"asan"})

    def test_public_header_does_not_add_compiler_or_firmware_suites(self):
        self.assertEqual(self.enabled(["include/ayni/ayni.h"]),
                         {"asan"})

    def test_kernel_events_and_sim_host_use_only_the_daily_suite(self):
        for path in ["tests/unit/kernel_event_test.cpp", "hosts/sim/sim_host.cpp",
                     "hosts/sim/include/ayni/hosts/sim.h"]:
            with self.subTest(path=path):
                self.assertEqual(self.enabled([path]), {"asan"})

    def test_build_and_ci_changes_do_not_expand_the_daily_matrix(self):
        for path in ["CMakePresets.json", "core/CMakeLists.txt", "cmake/AyniWarnings.cmake",
                     "tools/ci/run_tools.sh", "tools/ci/select_checks.py",
                     "tests/tools/CMakeLists.txt", ".github/workflows/ci.yml"]:
            with self.subTest(path=path):
                self.assertEqual(self.enabled([path]), {"asan"})

    def test_periodic_and_explicit_full_runs_cover_all_configurations(self):
        all_checks = {"asan", "release", "clang", "tsan", "esp_idf"}
        for event in ["schedule", "workflow_dispatch"]:
            self.assertEqual(self.enabled([], event), all_checks)
        self.assertEqual(self.enabled(["docs/workflow.md"], full=True), all_checks)

    def test_mixed_changes_still_select_only_one_native_suite(self):
        self.assertEqual(self.enabled(["docs/workflow.md", "hosts/native/file_log.cpp",
                                       "include/ayni/ayni.h"]),
                         {"asan"})

    def test_extra_native_configurations_are_one_matrix_job_each(self):
        self.assertEqual(selection.native_presets(selection.select_checks("schedule", [])),
                         ["native-release", "native-debug", "native-tsan"])
        self.assertEqual(
            selection.native_presets(selection.select_checks("pull_request", ["core/a.cpp"])), [])

    def test_merge_diff_includes_earlier_commits_and_old_paths_of_moves(self):
        with tempfile.TemporaryDirectory() as root:
            def git(*args):
                return subprocess.run(["git", *args], cwd=root, check=True,
                                      stdout=subprocess.PIPE, stderr=subprocess.PIPE)

            git("init", "-q", "-b", "main")
            git("config", "user.name", "CI test")
            git("config", "user.email", "ci@example.invalid")
            source = Path(root) / "core/kernel/ingress_lock.h"
            source.parent.mkdir(parents=True)
            source.write_text("// Existing lock\n")
            git("add", ".")
            git("commit", "-qm", "baseline")
            git("checkout", "-qb", "feature")
            git("mv", "core/kernel/ingress_lock.h", "core/retired_lock.md")
            git("commit", "-qm", "move lock")
            (Path(root) / "README.md").write_text("Documentation update\n")
            git("add", ".")
            git("commit", "-qm", "document move")
            git("checkout", "-q", "main")
            git("merge", "--no-ff", "-qm", "merge result", "feature")
            result = subprocess.run([sys.executable, str(SCRIPT), "pull_request"],
                                    cwd=root, check=True, capture_output=True, text=True)
            outputs = dict(line.split("=", 1) for line in result.stdout.splitlines())
            self.assertEqual(outputs["asan"], "true")
            self.assertEqual(outputs["tsan"], "false")
            self.assertEqual(outputs["esp_idf"], "false")
            self.assertEqual(outputs["native_presets"], "[]")


if __name__ == "__main__":
    unittest.main()
