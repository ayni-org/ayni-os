"""Normal fixture input boundaries and the compiled data contract (ADR-0068)."""
import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import check_contracts
import generate_normal_fixtures as fixtures


class NormalFixtureGenerator(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.base = json.loads((ROOT / "contracts/fixtures/normal/E1-N1.json").read_text())
        problems = []
        documents = check_contracts.parse_documents(ROOT, problems, set())
        cls.clauses = {c.id for document in documents for c in document.clauses}
        cls.steps = set(check_contracts.experiment_steps(ROOT))

    def load(self, value):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "E1-N1.json"
            path.write_text(json.dumps(value, ensure_ascii=False))
            return fixtures.load_case(path, self.clauses, self.steps)

    def test_actual_cases_have_unique_ids_and_generate_closed_cpp_data(self):
        ids, source = fixtures.generate(ROOT, list((ROOT / "contracts/fixtures/normal").glob("*.json")))
        self.assertEqual(len(ids), len(set(ids)))
        self.assertIn("const Case table[]", source)
        self.assertIn("SaveMemory{", source)
        self.assertIn("Expect{Metric::hold_status", source)
        self.assertNotIn("std::system", source)

    def test_unknown_fields_and_unimplemented_actions_are_explicitly_rejected(self):
        value = copy.deepcopy(self.base)
        value["external_url"] = "https://example.invalid"
        with self.assertRaises(fixtures.FixtureError):
            self.load(value)
        value = copy.deepcopy(self.base)
        value["script"] = [{"op": "storage_fault"}]
        with self.assertRaisesRegex(fixtures.FixtureError, "remain paused"):
            self.load(value)

    def test_object_references_and_local_event_targets_keep_their_kinds(self):
        value = copy.deepcopy(self.base)
        value["script"].append({"op": "revoke", "grant": "private"})
        with self.assertRaises(fixtures.FixtureError):
            self.load(value)
        value = copy.deepcopy(self.base)
        value["script"].append({"op": "local_input", "goal": "own"})
        with self.assertRaisesRegex(fixtures.FixtureError, "local Goal"):
            self.load(value)

    def test_assistant_device_group_is_never_silently_overridden(self):
        for group in (0, 2):
            value = copy.deepcopy(self.base)
            value["script"] = [{"op": "goal", "name": "check", "agent": 2,
                                "kind": "device_check", "pause_group": group}]
            with self.assertRaisesRegex(fixtures.FixtureError, "group 1"):
                self.load(value)

    def test_fake_time_and_step_bounds_refuse_ambiguous_numbers(self):
        for field, value in (("tick_us", 1000), ("tick_us", "-1"),
                             ("max_steps", True), ("step_units", 0)):
            data = copy.deepcopy(self.base)
            data["config"][field] = value
            with self.assertRaises(fixtures.FixtureError):
                self.load(data)

    def test_cpp_strings_preserve_utf8_bytes_without_surrogate_escapes(self):
        value = copy.deepcopy(self.base)
        value["script"][0]["body"] = "中文 🧪\n\x01"
        _, _, actions, _ = self.load(value)
        self.assertIn("\\360\\237", actions[0])
        self.assertIn("\\001", actions[0])
        self.assertNotIn("\\ud8", actions[0])

    def test_duplicate_keys_and_excessive_json_nesting_are_refused(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "E1-N1.json"
            path.write_text('{"case_id":"E1-N1","case_id":"E1-N2"}')
            with self.assertRaisesRegex(fixtures.FixtureError, "duplicate"):
                fixtures.load_case(path, self.clauses, self.steps)
            path.write_text('[' * 10 + '0' + ']' * 10)
            with self.assertRaisesRegex(fixtures.FixtureError, "nesting"):
                fixtures.load_case(path, self.clauses, self.steps)


if __name__ == "__main__":
    unittest.main()
