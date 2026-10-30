"""ADR-0075: CLI artifacts reflect actual runs and deterministically replay."""
import json
import re
from pathlib import Path
import subprocess
import sys
import tempfile


def verify_compatibility_matrix(matrix, artifacts):
    """Compare declared versions/outcomes with the last actual owner/query facts."""
    version_fields = {"storage", "strategy", "capability", "binding", "query_resolver",
                      "goal_type", "scene", "required_task_state", "required_task_state_max"}
    outcome_fields = {"task_state", "query_conclusion", "kernel_lifecycle", "scene_consumable", "control_phase"}
    for row in matrix["rows"]:
        evidence = [json.loads(line) for line in artifacts[row["case_id"]][1].splitlines()]
        facts = {(item["category"], item["name"]): item["value"] for item in evidence}
        assert isinstance(row["expected"], str) and row["expected"]  # Human-readable description.
        if "storage" in row["reader"]:
            # ADR-0085: manifests describe this binary, including injected-old-media cases.
            assert artifacts[row["case_id"]][0]["storage_schema"] == int(row["reader"]["storage"])
        for side in ("writer", "reader"):
            assert row[side] and set(row[side]) <= version_fields
            for field, expected in row[side].items():
                assert facts.get((side, field)) == expected, (row["case_id"], side, field, expected, facts.get((side, field)))
        assert row["outcome"] and set(row["outcome"]) <= outcome_fields
        for field, expected in row["outcome"].items():
            assert facts.get(("matrix", field)) == expected, (row["case_id"], field, expected)
        for name, expected in row["assertions"].items():
            assert any(item["category"] == "assertion" and item["name"] == name and item["value"] == expected for item in evidence), (row, name)


def main():
    executable = sys.argv[1]
    with tempfile.TemporaryDirectory(prefix="ayni-experiment-replay-") as directory:
        root = Path(directory)
        for suite in ("HARNESS", "E2", "E1", "E3", "E4"):
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
                elif suite == "E3":
                    canonical = (Path(__file__).resolve().parents[2] / "contracts/fixtures/cases.md").read_text()
                    assert set(re.findall(r"`(E3\.[^`]+)`", canonical)) == artifacts.keys()
                    assert all(value[0]["deployment"] == "replacement" for value in artifacts.values())
                    assert all("SYNTHETIC_PRIVATE_E3" not in value[1] for value in artifacts.values())
                elif suite == "E4":
                    repo = Path(__file__).resolve().parents[2]
                    canonical = (repo / "contracts/fixtures/cases.md").read_text()
                    required = set(re.findall(r"`(E4\.[^`]+)`", canonical)) - {
                        "E4.S1.schema_keyword_rejected", "E4.S6.migration_interrupted"}
                    assert required <= artifacts.keys()
                    assert all(value[0]["deployment"] == "composite" for value in artifacts.values())
                    assert all("SYNTHETIC_E4_SOURCE" not in value[1] for value in artifacts.values())
                    matrix = json.loads((repo / "contracts/fixtures/e4-compatibility.json").read_text())
                    assert matrix["schema"] == 1 and len(matrix["rows"]) >= 9
                    verify_compatibility_matrix(matrix, artifacts)
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
