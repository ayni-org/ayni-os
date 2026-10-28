"""ADR-0075: full-profile input boundaries cannot turn unsupported cases green."""
import copy
import json
import re
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import check_contracts
import generate_experiment_fixtures as fixtures


class ExperimentGenerator(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.base = json.loads((ROOT / "contracts/fixtures/experiments/HARNESS.S1.lost_uncommitted_create.json").read_text())
        problems = []
        documents = check_contracts.parse_documents(ROOT, problems, set())
        cls.clauses = {c.id for doc in documents for c in doc.clauses}
        cls.steps = set(check_contracts.experiment_steps(ROOT))

    def load(self, value, raw=None):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / (value["case_id"] + ".json")
            path.write_text(raw if raw is not None else json.dumps(value))
            parsed = fixtures.load_case(path, self.clauses, self.steps)
            return [fixtures.action(a) for a in parsed[3]]

    def test_repository_data_compiles_fault_and_restart_actions(self):
        source = fixtures.compile_cases(ROOT)
        self.assertIn("w.crash(hosts::sim::CutAppend::lost)", source)
        self.assertIn("w.service.execute(w.node.kernel())", source)
        self.assertNotIn("std::system", source)

    def test_delivered_e2_contains_every_original_canonical_variant(self):
        # ADR-0076: a suite cannot pass by omitting a difficult original variant.
        required = set(re.findall(r"`(E2\.[^`]+)`", (ROOT / "contracts/fixtures/cases.md").read_text()))
        actual = {path.stem for path in (ROOT / "contracts/fixtures/experiments").glob("E2.*.json")}
        self.assertGreater(len(required), 40)
        self.assertFalse(required - actual, sorted(required - actual))

    def test_delivered_e1_contains_every_active_canonical_variant(self):
        # ADR-0077/0042: retired cumulative quota IDs remain historical; the new
        # per-Task limit IDs must be present alongside every other original case.
        required = set(re.findall(r"`(E1\.[^`]+)`", (ROOT / "contracts/fixtures/cases.md").read_text()))
        required -= {"E1.S5.instance_quota_exhausted", "E1.S5.restart_keeps_consumption"}
        actual = {path.stem for path in (ROOT / "contracts/fixtures/experiments").glob("E1.*.json")}
        self.assertGreaterEqual(len(required), 31)
        self.assertFalse(required - actual, sorted(required - actual))

    def test_delivered_e4_covers_active_inventory_and_each_commit_disposition(self):
        # ADR-0079: JSON registration and migration are explicit user-approved
        # scope exceptions; every actual Core/typed and boundary case remains required.
        required = set(re.findall(r"`(E4\.[^`]+)`", (ROOT / "contracts/fixtures/cases.md").read_text()))
        required -= {"E4.S1.schema_keyword_rejected", "E4.S6.migration_interrupted"}
        actual = {p.stem for p in (ROOT / "contracts/fixtures/experiments").glob("E4.*.json")}
        self.assertFalse(required - actual, sorted(required - actual))
        boundaries = {"action_receipt", "capture_prepare", "capture_dispatch", "analysis_prepare", "analysis_dispatch", "analysis_receipt", "summary", "capture_stop", "presentation_prepare", "presentation_receipt", "task_complete"}
        for boundary in boundaries:
            for disposition in ("lost", "stored"):
                self.assertIn(f"E4.S2.{boundary}_{disposition}", actual)
        self.assertIn("E4.S1.typed_schema_rejected", actual)
        self.assertIn("E4.S6.unsupported_format_preserved", actual)
        self.assertIn("E4.S6.unsupported_record_version", actual)

    def test_composite_evidence_requires_actual_deployment_and_supported_cut(self):
        for action in ({"op": "composite", "scenario": "register_and_run"},
                       {"op": "expect", "metric": "composite_completed", "value": "1"}):
            value = copy.deepcopy(self.base)
            value["script"].insert(0, action)
            with self.assertRaises(fixtures.FixtureError):
                self.load(value)
        value = copy.deepcopy(self.base)
        value["deployment"] = "composite"
        value["script"].insert(0, {"op": "composite", "scenario": "restart_at_each_boundary", "boundary": "unimplemented", "append": "stored"})
        with self.assertRaises(fixtures.FixtureError):
            self.load(value)

    def test_shared_evidence_requires_its_actual_deployment_and_closed_scenario(self):
        for action in ({"op": "shared", "scenario": "local_rule_progresses"},
                       {"op": "expect", "metric": "shared_completed", "value": "1"}):
            data = copy.deepcopy(self.base)
            data["script"].insert(0, action)
            with self.assertRaises(fixtures.FixtureError):
                self.load(data)
        data = copy.deepcopy(self.base)
        data["deployment"] = "shared"
        data["script"].insert(0, {"op": "shared", "scenario": "skip_assertions"})
        with self.assertRaises(fixtures.FixtureError):
            self.load(data)

    def test_e3_inventory_and_deployment_are_enforced(self):
        # ADR-0078 / E3: replacement evidence requires actual models and policies.
        required = set(re.findall(r"`(E3\.[^`]+)`", (ROOT / "contracts/fixtures/cases.md").read_text()))
        actual = {p.stem for p in (ROOT / "contracts/fixtures/experiments").glob("E3.*.json")}
        self.assertEqual(required, actual)
        for action in ({"op": "replacement", "scenario": "baseline_checkpoint"},
                       {"op": "expect", "metric": "replacement_completed", "value": "1"}):
            data = copy.deepcopy(self.base)
            data["script"].insert(0, action)
            with self.assertRaises(fixtures.FixtureError):
                self.load(data)
        data = copy.deepcopy(self.base)
        data["deployment"] = "replacement"
        data["script"].insert(0, {"op": "replacement", "scenario": "discard_checkpoint"})
        with self.assertRaises(fixtures.FixtureError):
            self.load(data)

    def test_recovery_record_and_physical_controls_reject_unimplemented_inputs(self):
        for action in ({"op": "start", "expected_lifecycle": "FENCED"},
                       {"op": "budget", "items": 0}, {"op": "budget", "items": 65},
                       {"op": "hold_record", "point": "byte_offset_99"},
                       {"op": "damage", "region": "all_files"},
                       {"op": "fault_pending", "fault": "reject"},
                       {"op": "physical", "action": "at", "us": -1},
                       {"op": "physical", "action": "at", "us": 1000001},
                       {"op": "physical", "action": "prepare", "us": 1},
                       {"op": "physical", "action": "link", "mode": "real_device"},
                       {"op": "physical", "action": "deliver_renewal", "status": "DENIED"}):
            data = copy.deepcopy(self.base)
            data["script"].insert(0, action)
            with self.subTest(action=action), self.assertRaises(fixtures.FixtureError):
                self.load(data)
        for field, value in (("query_authority", "ASSUMED"), ("deployment", "hardware")):
            data = copy.deepcopy(self.base)
            data[field] = value
            with self.subTest(field=field), self.assertRaises(fixtures.FixtureError):
                self.load(data)

    def test_unknown_actions_fields_and_states_are_refused(self):
        for action in ({"op": "network_retry"}, {"op": "grant", "url": "secret"},
                       {"op": "expect", "metric": "lifecycle", "value": "MAGIC"},
                       {"op": "fault", "port": "append", "fault": "ignore_failure"}):
            data = copy.deepcopy(self.base)
            data["script"].insert(0, action)
            with self.assertRaises(fixtures.FixtureError):
                self.load(data)

    def test_physical_evidence_cannot_pass_on_an_absent_executor(self):
        for action in ({"op": "physical", "action": "prepare"},
                       {"op": "expect", "metric": "physical_submissions", "value": "0"}):
            data = copy.deepcopy(self.base)
            data["script"].insert(0, action)
            with self.assertRaises(fixtures.FixtureError):
                self.load(data)

    def test_original_draft_and_normal_profile_cannot_be_mislabelled_full(self):
        for key, value in (("fixture_schema", 0), ("fixture_schema", True),
                           ("profile", "m1-normal")):
            data = copy.deepcopy(self.base)
            data[key] = value
            with self.assertRaises(fixtures.FixtureError):
                self.load(data)

    def test_step_and_contract_metadata_must_exist(self):
        data = copy.deepcopy(self.base)
        data["case_id"] = "E1.S99.missing_step"
        data["experiment_step"] = "E1 步骤 99"
        with self.assertRaises(fixtures.FixtureError):
            self.load(data)
        data = copy.deepcopy(self.base)
        data["clauses"] = ["C-MISSING-999"]
        with self.assertRaises(fixtures.FixtureError):
            self.load(data)

    def test_counts_reject_boolean_zero_and_unbounded_waits(self):
        for action in ({"op": "steps", "count": True}, {"op": "steps", "count": 0},
                       {"op": "until", "metric": "pending_calls", "value": "1", "bound": 1001},
                       {"op": "expect", "metric": "executions", "value": 1}):
            data = copy.deepcopy(self.base)
            data["script"].append(action)
            with self.assertRaises(fixtures.FixtureError):
                self.load(data)

    def test_empty_scripts_and_missing_assertions_never_generate_passing_cases(self):
        for script in ([], [{"op": "grant"}]):
            data = copy.deepcopy(self.base)
            data["script"] = script
            with self.assertRaises(fixtures.FixtureError):
                self.load(data)

    def test_duplicate_keys_nesting_and_noninteger_numbers_are_refused(self):
        for raw in ('{"case_id":"a","case_id":"b"}', '[' * 10 + '0' + ']' * 10,
                    '{"value":NaN}', '{"value":1.5}'):
            with self.assertRaises(fixtures.FixtureError):
                self.load(self.base, raw)


if __name__ == "__main__":
    unittest.main()
