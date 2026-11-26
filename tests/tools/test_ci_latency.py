"""Check latency isolation after CTest's deferred GoogleTest discovery."""
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
CASES = {
    "ayni_unit_tests": [
        "ControlSessionTest.LocalLoopWallLatencyMeetsHostBudget",
        "ReflexParameters.DynamicProjectionRechecksScopeAndMeetsHostLatencyBudget",
    ],
    "ayni_esp32_chat_integration_tests": [
        "ControlMailbox.TargetReceiptWallLatencyMeetsHostBudget",
        "TurnDiagnostics.OwnerWallLatencyReportsShortMediumAndLongHistories",
    ],
    "ayni_esp32_motor_execution_tests": [
        "MotorWorker.SoftwareHotSameDirectionReverseAndHoldHaveBoundedMeasuredLatency",
    ],
}


@unittest.skipUnless(all(shutil.which(tool) for tool in ("cmake", "ctest", "ninja")),
                     "CMake/CTest/Ninja required")
class CiLatencyTest(unittest.TestCase):
    def test_pre_test_discovery_isolates_latency_cases_and_keeps_ordinary_cases_parallel(self):
        # PRE_TEST registers cases at CTest runtime, not at CMake configure time.
        # Listing-only stand-ins exercise real GoogleTest discovery without compiling Core.
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory)
            cmake = ["cmake_minimum_required(VERSION 3.22)",
                     "project(latency_registration NONE)", "enable_testing()",
                     "include(GoogleTest)"]
            for target, cases in CASES.items():
                listing = "\n".join(case.replace(".", ".\n  ", 1) for case in cases)
                listing += "\nOrdinary.\n  " + target
                executable = source / target
                executable.write_text("#!/usr/bin/env python3\nprint(" + repr(listing) + ")\n")
                executable.chmod(0o755)
                cmake.extend([
                    f"add_executable({target} IMPORTED)",
                    f'set_target_properties({target} PROPERTIES IMPORTED_LOCATION "{executable}")',
                    f"gtest_discover_tests({target} DISCOVERY_MODE PRE_TEST)",
                ])
            cmake.append('set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES '
                         f'"{ROOT / "tests/support/host_latency_tests.cmake"}")')
            (source / "CMakeLists.txt").write_text("\n".join(cmake) + "\n")
            # CI installs Ninja, not Make; use the same explicit generator as its presets.
            configured = subprocess.run(["cmake", "-G", "Ninja", "-S", str(source),
                                         "-B", str(source / "build")],
                                        capture_output=True, text=True)
            self.assertEqual(configured.returncode, 0, configured.stdout + configured.stderr)
            result = subprocess.run(["ctest", "--test-dir", str(source / "build"),
                                     "--show-only=json-v1"], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            tests = json.loads(result.stdout)["tests"]
            expected_latency = {case for cases in CASES.values() for case in cases}
            self.assertEqual({test["name"] for test in tests}, expected_latency |
                             {"Ordinary." + target for target in CASES})
            for test in tests:
                with self.subTest(case=test["name"]):
                    properties = {prop["name"]: prop["value"] for prop in test["properties"]}
                    if test["name"] in expected_latency:
                        self.assertIs(properties.get("RUN_SERIAL"), True)
                        self.assertIn("host_latency", properties.get("LABELS", []))
                    else:
                        self.assertFalse(properties.get("RUN_SERIAL", False))
                        self.assertNotIn("host_latency", properties.get("LABELS", []))


if __name__ == "__main__":
    unittest.main()
