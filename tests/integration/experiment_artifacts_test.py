"""ADR-0075: CLI artifacts reflect actual runs and deterministically replay."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    executable = sys.argv[1]
    with tempfile.TemporaryDirectory(prefix="ayni-experiment-replay-") as directory:
        root = Path(directory)
        outputs = []
        for iteration in range(2):
            output = root / str(iteration)
            subprocess.run([executable, "--experiment", "HARNESS", "--output", str(output)],
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
                assert any(item["category"] == "fault" for item in records)
                assert any(item["category"] == "assertion" for item in records)
                assert all(set(item) == {"category", "name", "value", "sequence", "step", "clock_us"}
                           for item in records)
                assert [int(item["sequence"]) for item in records] == list(range(1, len(records) + 1))
                artifacts[case.name] = (manifest, trace)
            assert len(artifacts) >= 5
            outputs.append(artifacts)
        assert outputs[0] == outputs[1], "same compiled case produced different logical evidence"
        # Existing directories and unknown suites must not be reported as successful.
        for args in (("--experiment", "HARNESS", "--output", str(root / "0")),
                     ("--experiment", "UNIMPLEMENTED", "--output", str(root / "missing"))):
            result = subprocess.run([executable, *args], timeout=30)
            assert result.returncode != 0
        assert not (root / "missing").exists()
    return 0


if __name__ == "__main__":
    sys.exit(main())
