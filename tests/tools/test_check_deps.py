"""Scenarios for tools/check_deps.py, each on a throwaway repository tree."""

import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import check_deps  # noqa: E402

ARCHITECTURE = json.loads((ROOT / "tools" / "architecture.json").read_text(encoding="utf-8"))


class CheckDepsTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self.architecture = json.loads(json.dumps(ARCHITECTURE))

    def tearDown(self):
        self._tmp.cleanup()

    def write(self, relative, text=""):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def violations(self):
        return check_deps.check(self.root, check_deps.Architecture(self.architecture))

    def assert_single_violation(self, fragment):
        found = self.violations()
        self.assertEqual(len(found), 1, found)
        self.assertIn(fragment, found[0])

    def test_core_includes_lower_layer(self):
        self.write("core/work/task/task.cpp", '#include "foundation/ids.h"\n#include "budget/budget.h"\n')
        self.assertEqual(self.violations(), [])

    def test_core_rejects_upward_include(self):
        self.write("core/foundation/ids.cpp", '#include "kernel/kernel.h"\n')
        self.assert_single_violation("core/foundation may not include core/kernel")

    def test_core_rejects_same_layer_include(self):
        self.write("core/authority/grant/grant.cpp", '#include "budget/budget.h"\n')
        self.assert_single_violation("core/authority may not include core/budget")

    def test_core_rejects_relative_path_to_higher_layer(self):
        self.write("core/kernel/kernel.h")
        self.write("core/catalog/device/device.cpp", '#include "../../kernel/kernel.h"\n')
        self.assert_single_violation("core/catalog may not include core/kernel")

    def test_core_subdir_include_requires_declaration(self):
        self.write("core/work/task/task.cpp", '#include "work/call/call.h"\n')
        self.assert_single_violation("core/work/task may not include core/work/call")
        self.architecture["core_subdir_deps"] = {"work": {"task": ["call"]}}
        self.assertEqual(self.violations(), [])

    def test_core_module_root_reaches_own_subdirs(self):
        self.write("core/work/work.cpp", '#include "work/task/task.h"\n#include "work/call/call.h"\n')
        self.assertEqual(self.violations(), [])

    def test_core_rejects_platform_and_unlisted_std_headers(self):
        self.write("core/kernel/kernel.cpp", "#include <vector>\n#include <freertos/FreeRTOS.h>\n#include <cstdint>\n")
        found = self.violations()
        self.assertEqual(len(found), 2, found)
        self.assertIn("<vector>", found[0])
        self.assertIn("<freertos/FreeRTOS.h>", found[1])

    def test_core_rejects_draft_host_and_runtime_headers(self):
        self.write(
            "core/kernel/kernel.cpp",
            "#include <ayni/ayni.h>\n#include <ayni/draft/host_v1.h>\n"
            "#include <ayni/hosts/native.h>\n#include <ayni/runtime/native_node.h>\n",
        )
        found = self.violations()
        self.assertEqual(len(found), 3, found)

    def test_core_sources_outside_modules_are_rejected(self):
        self.write("core/util/strings.cpp")
        self.assert_single_violation("must live in one of the modules")

    def test_public_header_rejects_draft_include(self):
        self.write("include/ayni/ayni.h", "#include <stdint.h>\n#include <ayni/draft/host_v1.h>\n")
        self.assert_single_violation("implemented public header may not include <ayni/draft/host_v1.h>")

    def test_draft_header_may_include_api_and_drafts(self):
        self.write("include/ayni/draft/host_v1.h", '#include <ayni/ayni.h>\n#include "kernel_api.h"\n')
        self.write("include/ayni/draft/kernel_api.h")
        self.assertEqual(self.violations(), [])

    def test_target_uses_runtime_not_hosts(self):
        self.write("targets/simulator/main.cpp", "#include <ayni/runtime/native_node.h>\n#include <cstdio>\n")
        self.assertEqual(self.violations(), [])
        self.write("targets/simulator/main.cpp", "#include <ayni/hosts/native.h>\n")
        self.assert_single_violation("targets/ may not include <ayni/hosts/native.h>")

    def test_only_runtime_includes_hosts_across_components(self):
        self.write("runtime/native_node/node.cpp", "#include <ayni/hosts/native.h>\n")
        self.assertEqual(self.violations(), [])

    def test_host_includes_own_headers_but_not_other_hosts(self):
        self.write("hosts/native/native_host.cpp", "#include <ayni/hosts/native.h>\n")
        self.assertEqual(self.violations(), [])
        self.write("hosts/native/native_host.cpp", "#include <ayni/hosts/esp_idf.h>\n")
        self.assert_single_violation("hosts/ may not include <ayni/hosts/esp_idf.h>")

    def test_runtime_does_not_include_other_runtime(self):
        self.write("runtime/esp_idf_node/node.cpp", "#include <ayni/runtime/native_node.h>\n")
        self.assert_single_violation("runtime/ may not include <ayni/runtime/native_node.h>")

    def test_core_internal_headers_never_leave_core(self):
        self.write("runtime/native_node/node.cpp", '#include "kernel/kernel.h"\n')
        self.write("control/permit_gate.cpp", '#include "resources/permit/permit.h"\n')
        found = self.violations()
        self.assertEqual(len(found), 2, found)
        self.assertTrue(all("Core-internal header" in line for line in found), found)

    def test_relative_include_into_another_component_is_foreign(self):
        self.write("hosts/native/native_host.h")
        self.write("targets/simulator/main.cpp", '#include "../../hosts/native/native_host.h"\n')
        self.assert_single_violation("file of hosts/native/")

    def test_local_quoted_includes_are_allowed(self):
        self.write("safety/stop_latch.h")
        self.write("safety/stop_latch.cpp", '#include "stop_latch.h"\n')
        self.assertEqual(self.violations(), [])

    def test_tests_are_unchecked_and_build_dirs_skipped(self):
        self.write("tests/unit/kernel_test.cpp", '#include "kernel/kernel.h"\n')
        self.write("targets/esp32_assistant/build/generated.c", "#include <ayni/hosts/esp_idf.h>\n")
        self.assertEqual(self.violations(), [])

    def test_unknown_top_level_directory_needs_a_rule(self):
        self.write("providers/model/mock.cpp")
        self.assert_single_violation("no include rule for providers/")

    def test_duplicate_core_module_is_a_configuration_error(self):
        self.architecture["core_layers"].append(["kernel"])
        with self.assertRaises(check_deps.ConfigError):
            check_deps.Architecture(self.architecture)

    def test_line_numbers_point_at_the_include(self):
        self.write("core/foundation/ids.cpp", "// comment\n\n#include <vector>\n")
        self.assertTrue(self.violations()[0].startswith("core/foundation/ids.cpp:3:"))


if __name__ == "__main__":
    unittest.main()
