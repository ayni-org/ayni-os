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
        for suite in ("HARNESS", "E2", "E1"):
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
                    assert all(int(item["value"]) <= int(item["clock_us"])
                               for item in records if item["name"] in {
                                   "execution:stop_confirmed_at_us", "invalidated_at_us"})
                    if case.name == "HARNESS.S1.crash_active_executor":
                        assert any(item["name"] == "execution:phase" and item["value"] == "4"
                                   for item in records), "close must record independent STOPPED"
                        assert any(item["name"] == "execution:stop_confirmed_at_us" and int(item["value"]) > 0
                                   for item in records)
                    if case.name == "E1.S6.revoke_in_flight":
                        phases = ("ACCEPTED_phase", "DURABLY_EFFECTIVE_seq", "CANCELLING_call", "STOP_CONFIRMED_seq")
                        observed = [next(int(row["sequence"]) for row in records if row["category"] == "revoke" and row["name"] == name and int(row["value"]) > 0) for name in phases]
                        assert observed == sorted(observed), "revoke confirmation phases must remain distinguishable"
                    if case.name == "E1.S9.cleanup_resumes_after_restart":
                        privacy = {row["name"]: int(row["value"]) for row in records if row["category"] == "privacy"}
                        assert 0 < privacy["cut_phase"] < 4 and 0 < privacy["recovered_phase"] < 4
                        assert privacy["cut_request_seq"] == privacy["recovered_request_seq"] > 0
                    assert [int(item["sequence"]) for item in records] == list(range(1, len(records) + 1))
                    artifacts[case.name] = (manifest, trace)
                if suite == "E2":
                    canonical = (Path(__file__).resolve().parents[2] / "contracts/fixtures/cases.md").read_text()
                    assert set(re.findall(r"`(E2\.[^`]+)`", canonical)) <= artifacts.keys()
                    assert all(value[0]["deployment"] in {"counter", "physical"} for value in artifacts.values())
                elif suite == "E1":
                    canonical = (Path(__file__).resolve().parents[2] / "contracts/fixtures/cases.md").read_text()
                    required = set(re.findall(r"`(E1\.[^`]+)`", canonical)) - {
                        "E1.S5.instance_quota_exhausted", "E1.S5.restart_keeps_consumption"}
                    assert required <= artifacts.keys()
                    assert all(value[0]["deployment"] in {"shared", "physical", "shared_physical"} for value in artifacts.values())
                    latency = [json.loads(line) for line in artifacts["E1.S2.local_rule_progresses"][1].splitlines()]
                    assert any(row["name"] == "local_response_steps" and 0 < int(row["value"]) <= 10 for row in latency)
                    for manifest, trace in artifacts.values():
                        assert "SYNTHETIC_PRIVATE_" not in trace and "SYNTHETIC_PROFILE_" not in trace
                        assert "SYNTHETIC_SOURCE_" not in trace and "SYNTHETIC_USER_NEW_VERSION_" not in trace
                else:
                    assert len(artifacts) == 7
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
