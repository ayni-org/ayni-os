"""ADR-0075: full-profile input boundaries cannot turn unsupported cases green."""
import copy
import json
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

    def test_unknown_actions_fields_and_states_are_refused(self):
        for action in ({"op": "network_retry"}, {"op": "grant", "url": "secret"},
                       {"op": "expect", "metric": "lifecycle", "value": "MAGIC"},
                       {"op": "fault", "port": "append", "fault": "ignore_failure"}):
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
