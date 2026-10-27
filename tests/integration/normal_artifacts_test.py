"""Run compiled normal fixtures twice and verify their actual portable artifacts (ADR-0068)."""
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

SIMULATOR = Path(sys.argv.pop(1))
ARTIFACT_ROOT = Path(sys.argv.pop(1))


class NormalArtifacts(unittest.TestCase):
    def test_replays_produce_identical_redacted_json_traces(self):
        listing = subprocess.run([str(SIMULATOR), "--list-normal"], check=True,
                                 capture_output=True, text=True)
        cases = [line.split("\t")[0] for line in listing.stdout.splitlines()]
        self.assertTrue(cases)
        self.assertEqual(len(cases), len(set(cases)))
        ARTIFACT_ROOT.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="replay-", dir=ARTIFACT_ROOT) as temporary:
            for case in cases:
                with self.subTest(case=case):
                    traces = []
                    for repeat in range(2):
                        directory = Path(temporary) / f"{case}-{repeat}"
                        process = subprocess.run(
                            [str(SIMULATOR), "--case", case, "--output", str(directory)],
                            capture_output=True, text=True, timeout=30)
                        self.assertEqual(process.returncode, 0, process.stdout + process.stderr)
                        manifest_text = (directory / "run.json").read_text()
                        trace_text = (directory / "trace.jsonl").read_text()
                        self.assertNotIn("SYNTHETIC_", manifest_text + trace_text)
                        manifest = json.loads(manifest_text)
                        self.assertEqual(manifest["case_id"], case)
                        self.assertEqual(manifest["profile"], "m1-normal")
                        self.assertTrue(manifest["passed"])
                        self.assertEqual(manifest["status"], 0)
                        self.assertEqual(manifest["close_status"], 0)
                        self.assertEqual(manifest["artifact_status"], 0)
                        self.assertEqual(manifest["fault_recovery"], "paused_unverified")
                        self.assertEqual(manifest["hardware"], "unverified")
                        self.assertTrue(manifest["revision"])
                        header = (Path(__file__).resolve().parents[2] / "include/ayni/ayni.h").read_text()
                        config = header.split("typedef struct ayni_config_v1 {")[1].split("} ayni_config_v1;")[0]
                        fields = set(re.findall(r"uint32_t\s+(\w+)\s*;", config))
                        self.assertEqual(set(manifest["kernel_config"]), fields)
                        records = [json.loads(line) for line in trace_text.splitlines()]
                        self.assertTrue(records)
                        clock = step = 0
                        for sequence, record in enumerate(records, start=1):
                            self.assertEqual(record["sequence"], str(sequence))
                            self.assertGreaterEqual(int(record["clock_us"]), clock)
                            self.assertGreaterEqual(int(record["step"]), step)
                            clock, step = int(record["clock_us"]), int(record["step"])
                            self.assertFalse({"body", "text", "token", "password", "media_bytes"} & record.keys())
                            for key in ("sequence", "step", "clock_us", "commit_seq", "operation_id",
                                        "logical_call_id", "request_id", "boot_epoch", "transition"):
                                if key in record:
                                    self.assertIsInstance(record[key], str)
                                    self.assertGreaterEqual(int(record[key]), 0)
                        self.assertIn("command", {row["category"] for row in records})
                        self.assertIn("assertion", {row["category"] for row in records})
                        traces.append(trace_text)
                    self.assertEqual(traces[0], traces[1], "fixed-order normal replay drifted")


if __name__ == "__main__":
    unittest.main()
