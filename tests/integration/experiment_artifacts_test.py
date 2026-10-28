"""ADR-0075: CLI artifacts reflect actual runs and deterministically replay."""
import json
import re
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    executable = sys.argv[1]
    with tempfile.TemporaryDirectory(prefix="ayni-experiment-replay-") as directory:
        root = Path(directory)
        for suite in ("HARNESS", "E2"):
            outputs = []
            for iteration in range(2):
                output = root / (suite + str(iteration))
                subprocess.run([executable, "--experiment", suite, "--output", str(output)],
                               check=True, timeout=30)
                artifacts = {}
                for case in sorted(output.iterdir()):
                    manifest = json.loads((case / "run.json").read_text())
                    assert manifest["case_id"] == case.name
                    assert manifest["profile"] == "m1-experiment"
                    assert manifest["passed"] is True
                    assert manifest["status"] == manifest["close_status"] == manifest["artifact_status"] == 0
                    assert manifest["first_failure"] == ""
                    assert manifest["hardware"] == "unverified"
                    assert manifest["revision"] and manifest["compiler"] and manifest["kernel_config"]
                    trace = (case / "trace.jsonl").read_text()
                    records = [json.loads(line) for line in trace.splitlines()]
                    if suite == "HARNESS":
                        assert any(item["category"] == "fault" for item in records)
                    assert any(item["category"] == "assertion" for item in records)
                    assert all(set(item) == {"category", "name", "value", "sequence", "step", "clock_us"}
                               for item in records)
                    assert [int(item["sequence"]) for item in records] == list(range(1, len(records) + 1))
                    artifacts[case.name] = (manifest, trace)
                if suite == "E2":
                    canonical = (Path(__file__).resolve().parents[2] / "contracts/fixtures/cases.md").read_text()
                    assert set(re.findall(r"`(E2\.[^`]+)`", canonical)) <= artifacts.keys()
                    assert all(value[0]["deployment"] in {"counter", "physical"} for value in artifacts.values())
                else:
                    assert len(artifacts) == 6
                outputs.append(artifacts)
            assert outputs[0] == outputs[1], "same compiled case produced different logical evidence"
        # Existing directories and unknown suites must not be reported as successful.
        for args in (("--experiment", "HARNESS", "--output", str(root / "HARNESS0")),
                     ("--experiment", "UNIMPLEMENTED", "--output", str(root / "missing"))):
            result = subprocess.run([executable, *args], timeout=30)
            assert result.returncode != 0
        assert not (root / "missing").exists()
    return 0


if __name__ == "__main__":
    sys.exit(main())
