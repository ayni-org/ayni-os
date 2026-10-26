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
        self.write("core/work/task/task.cpp", '#include "foundation/ids.h"\n#include "resources/resource.h"\n')
        self.assertEqual(self.violations(), [])

    def test_provider_own_header_and_runtime_composition_are_allowed(self):
        self.write("providers/mock/mock.cpp", "#include <ayni/providers/mock.h>\n#include <ayni/calls.h>\n")
        self.write("runtime/native_node/node.cpp", "#include <ayni/providers/mock.h>\n")
        self.assertEqual(self.violations(), [])

    def test_provider_cannot_include_another_provider_or_core_internal(self):
        self.write("providers/mock/mock.cpp", "#include <ayni/providers/network.h>\n")
        self.assert_single_violation("providers/ may not include")
        self.write("providers/mock/mock.cpp", '#include "kernel/runtime/kernel_state.h"\n')
        self.assert_single_violation("Core-internal header")

    def test_strategies_use_only_public_component_boundaries(self):
        self.write("strategies/builtin/loop.cpp", "#include <ayni/strategies/builtin.h>\n#include <ayni/calls.h>\n")
        self.write("runtime/native_node/node.cpp", "#include <ayni/strategies/builtin.h>\n")
        self.assertEqual(self.violations(), [])
        self.write("strategies/builtin/loop.cpp", '#include "kernel/runtime/kernel_state.h"\n')
        self.assert_single_violation("Core-internal header")
        self.write("strategies/builtin/loop.cpp", "#include <ayni/providers/mock.h>\n")
        self.assert_single_violation("strategies/ may not include")

    def test_core_rejects_upward_include(self):
        self.write("core/foundation/ids.cpp", '#include "kernel/kernel.h"\n')
        self.assert_single_violation("core/foundation may not include core/kernel")

    def test_core_rejects_same_layer_include(self):
        self.write("core/authority/grant/grant.cpp", '#include "resources/resource.h"\n')
        self.assert_single_violation("core/authority may not include core/resources")

    def test_core_rejects_relative_path_to_higher_layer(self):
        self.write("core/kernel/kernel.h")
        self.write("core/catalog/device/device.cpp", '#include "../../kernel/kernel.h"\n')
        self.assert_single_violation("core/catalog may not include core/kernel")

    def test_core_subdir_include_requires_declaration(self):
        self.write("core/work/task/task.cpp", '#include "work/call/call.h"\n')
        self.assert_single_violation("core/work/task may not include core/work/call")
        self.architecture["core_subdir_deps"] = {"work": {"task": ["call"]}}
        self.assertEqual(self.violations(), [])

    def test_core_module_root_includes_only_declared_subdirs(self):
        # ADR-0051: the module root is one more node of the declared graph.
        self.write("core/work/work.cpp", '#include "work/task/task.h"\n')
        self.assertEqual(self.violations(), [])
        self.write("core/work/work.cpp", '#include "work/action/action.h"\n')
        self.assert_single_violation("the root of core/work ('_root') may not include core/work/action")

    def test_core_subdir_reaches_module_root_only_when_declared(self):
        # ADR-0051: the blind spot through the root, spelled from the include root and relatively.
        self.write("core/kernel/kernel_state.h")
        self.write("core/kernel/intent/rules.cpp", '#include "kernel/kernel_state.h"\n')
        self.assert_single_violation("core/kernel/intent may not include the root of core/kernel")
        self.write("core/kernel/intent/rules.cpp", '#include "../kernel_state.h"\n')
        self.assert_single_violation("core/kernel/intent may not include the root of core/kernel")
        self.architecture["core_subdir_deps"] = {"kernel": {"intent": ["_root"]}}
        self.assertEqual(self.violations(), [])

    def test_nested_kernel_partitions_preserve_allowed_service_calls(self):
        # ADR-0058: a runtime depends on a service; services share lower ports.
        self.write("core/kernel/runtime/step.cpp", '#include "kernel/service/model/work_model.h"\n')
        self.write("core/kernel/service/call/call.cpp", '#include "kernel/ports/provider_mailbox.h"\n')
        self.assertEqual(self.violations(), [])

    def test_nested_service_cannot_include_runtime_even_by_relative_path(self):
        # ADR-0058: resolving a relative include cannot bypass the partition boundary.
        self.write("core/kernel/runtime/kernel_state.h")
        self.write("core/kernel/service/model/model.cpp", '#include "../../runtime/kernel_state.h"\n')
        self.assert_single_violation("core/kernel/service/model may not include core/kernel/runtime")

    def test_longest_declared_nested_boundary_keeps_service_internals_separate(self):
        # ADR-0058: declaring a common parent cannot erase its stricter child rule.
        self.architecture["core_subdir_deps"]["kernel"]["service"] = ["service/decision"]
        self.write("core/kernel/service/model/model.cpp", '#include "kernel/service/decision/decision.h"\n')
        self.assert_single_violation("core/kernel/service/model may not include core/kernel/service/decision")

    def test_include_root_alias_cannot_hide_runtime_behind_allowed_ports(self):
        # ADR-0058: canonical and dot-segment include spellings share a boundary,
        # including headers absent from a fixture or generated at build time.
        for exists in (False, True):
            if exists:
                self.write("core/kernel/runtime/kernel_state.h")
            for path in ("kernel/runtime/kernel_state.h", "kernel/ports/../runtime/kernel_state.h"):
                with self.subTest(exists=exists, path=path):
                    self.write("core/kernel/service/model/model.cpp", f'#include "{path}"\n')
                    self.assert_single_violation("core/kernel/service/model may not include core/kernel/runtime")

    def test_nested_partition_cycle_is_rejected(self):
        # ADR-0058: nested names participate in the same deterministic cycle check.
        self.architecture["core_subdir_deps"] = {
            "kernel": {"runtime": ["service/model"], "service/model": ["runtime"]}}
        with self.assertRaisesRegex(check_deps.ConfigError, "has a cycle"):
            check_deps.Architecture(self.architecture)

    def test_cycle_in_declared_subdir_graph_is_a_configuration_error(self):
        # ADR-0051: a declared cycle through the root, a longer one and a self-loop.
        for graph, cycle in (({"_root": ["intent"], "intent": ["_root"]}, "_root -> intent -> _root"),
                             ({"a": ["b"], "b": ["c"], "c": ["a"]}, "a -> b -> c -> a"),
                             ({"a": ["a"]}, "a -> a")):
            with self.subTest(cycle=cycle):
                self.architecture["core_subdir_deps"] = {"kernel": graph}
                with self.assertRaisesRegex(check_deps.ConfigError, f"'kernel' has a cycle: {cycle}"):
                    check_deps.Architecture(self.architecture)

    def test_acyclic_declared_subdir_graph_with_shared_targets_is_valid(self):
        self.architecture["core_subdir_deps"] = {"kernel": {"_root": ["a", "b"], "a": ["c"], "b": ["c"]}}
        check_deps.Architecture(self.architecture)

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

    def test_every_runtime_shares_runtime_common_only(self):
        # ADR-0053: one named instance, not every other runtime.
        self.write("runtime/native_node/node.cpp",
                   "#include <ayni/runtime/common/default_config.h>\n")
        self.write("runtime/esp_idf_node/node.cpp",
                   "#include <ayni/runtime/common/default_config.h>\n")
        self.assertEqual(self.violations(), [])
        self.write("hosts/native/native_host.cpp",
                   "#include <ayni/runtime/common/default_config.h>\n")
        self.assert_single_violation("hosts/ may not include <ayni/runtime/common/default_config.h>")

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

    def test_every_cpp_suffix_is_checked(self):
        for suffix in (".hh", ".hxx", ".inl", ".ipp", ".tpp", ".ixx", ".cxx"):
            with self.subTest(suffix=suffix):
                self.write(f"core/foundation/ids{suffix}", '#include "kernel/kernel.h"\n')
                self.assert_single_violation("core/foundation may not include core/kernel")
                (self.root / f"core/foundation/ids{suffix}").unlink()

    def test_host_own_header_with_hpp_suffix_is_own(self):
        self.write("hosts/native/native_host.cpp", "#include <ayni/hosts/native.hpp>\n")
        self.assertEqual(self.violations(), [])

    def test_unrecognized_file_type_in_core_or_include_is_rejected(self):
        self.write("core/kernel/README.md")
        self.write("core/kernel/CMakeLists.txt")
        self.write("include/ayni/layers.cmake")
        self.write("hosts/native/notes.def")
        self.assertEqual(self.violations(), [])
        self.write("core/kernel/table.def", '#include "work/task/task.h"\n')
        self.assert_single_violation("core/kernel/table.def: file type '.def' is not read")

    def test_non_utf8_source_is_a_violation(self):
        path = self.root / "core/kernel/kernel.cpp"
        path.parent.mkdir(parents=True)
        path.write_bytes(b"// caf\xe9\n#include <vector>\n")
        self.assert_single_violation("core/kernel/kernel.cpp: not UTF-8")

    def test_unknown_top_level_directory_needs_a_rule(self):
        self.write("unplanned/module.cpp")
        self.assert_single_violation("no include rule for unplanned/")

    def test_duplicate_core_module_is_a_configuration_error(self):
        self.architecture["core_layers"].append(["kernel"])
        with self.assertRaises(check_deps.ConfigError):
            check_deps.Architecture(self.architecture)

    def test_line_numbers_point_at_the_include(self):
        self.write("core/foundation/ids.cpp", "// comment\n\n#include <vector>\n")
        self.assertTrue(self.violations()[0].startswith("core/foundation/ids.cpp:3:"))


if __name__ == "__main__":
    unittest.main()
