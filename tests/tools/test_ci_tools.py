"""Exercise CI cache publication and process boundaries without a Docker daemon."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class CiToolsTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.repo = self.root / "checkout with spaces"
        self.repo.mkdir()
        source = Path(__file__).resolve().parents[2] / "tools" / "ci"
        shutil.copytree(source, self.repo / "tools" / "ci")
        subprocess.run(["git", "init", "-q", str(self.repo)], check=True)
        self.bin = self.root / "bin"
        self.bin.mkdir()
        self.log = self.root / "calls.jsonl"
        docker = self.bin / "docker"
        docker.write_text('''#!/usr/bin/env python3
import json, os, pathlib, sys
args = sys.argv[1:]
with open(os.environ["FAKE_CALLS"], "a") as log:
    log.write(json.dumps(args) + "\\n")
state = pathlib.Path(os.environ["FAKE_IMAGE"])
if args[:2] == ["image", "inspect"]:
    sys.exit(0 if state.exists() and state.read_text() == args[2] else 1)
if args[0] == "create":
    print("bootstrap-container")
elif args[0] == "inspect":
    print(os.environ.get("FAKE_INSTALL_STATUS", "0"))
elif args[0] == "commit":
    status = int(os.environ.get("FAKE_COMMIT_STATUS", "0"))
    if not status:
        state.write_text(args[-1])
    sys.exit(status)
elif args[0] == "run":
    sys.exit(int(os.environ.get("FAKE_RUN_STATUS", "0")))
''')
        docker.chmod(0o755)
        self.env = dict(os.environ, PATH=f"{self.bin}:{os.environ['PATH']}",
                        GITHUB_WORKSPACE=str(self.repo), FAKE_CALLS=str(self.log),
                        FAKE_IMAGE=str(self.root / "image"))
        self.script = self.repo / "tools" / "ci" / "run_tools.sh"

    def run_script(self, *args, cwd=None, **env):
        return subprocess.run([str(self.script), *args], cwd=cwd or self.repo,
                              env=dict(self.env, **env), text=True, capture_output=True)

    def calls(self):
        return [json.loads(line) for line in self.log.read_text().splitlines()]

    def test_warm_recipe_skips_install_and_changed_recipe_rebuilds(self):
        self.assertEqual(self.run_script("prepare").returncode, 0)
        self.assertEqual(self.run_script("prepare").returncode, 0)
        self.assertEqual(sum(c[0] == "create" for c in self.calls()), 1)
        self.assertEqual(sum(c[0] == "pull" for c in self.calls()), 1)
        recipe = self.repo / "tools" / "ci" / "install_tools.sh"
        recipe.write_text(recipe.read_text() + "\n# Refresh recipe\n")
        self.assertEqual(self.run_script("prepare").returncode, 0)
        self.assertEqual(sum(c[0] == "create" for c in self.calls()), 2)

    def test_failed_install_is_cleaned_up_without_publishing_an_image(self):
        result = self.run_script("prepare", FAKE_INSTALL_STATUS="42")
        self.assertEqual(result.returncode, 42, result.stderr)
        self.assertFalse(any(c[0] == "commit" for c in self.calls()))
        self.assertIn(["rm", "-f", "bootstrap-container"], self.calls())
        self.assertEqual(self.run_script("prepare").returncode, 0)
        self.assertEqual(sum(c[0] == "create" for c in self.calls()), 2)

    def test_failed_commit_is_not_reused_and_container_is_removed(self):
        result = self.run_script("prepare", FAKE_COMMIT_STATUS="43")
        self.assertEqual(result.returncode, 43, result.stderr)
        self.assertIn(["rm", "-f", "bootstrap-container"], self.calls())
        self.assertEqual(self.run_script("prepare").returncode, 0)
        self.assertEqual(sum(c[0] == "create" for c in self.calls()), 2)

    def test_run_preserves_arguments_environment_path_and_failure(self):
        nested = self.repo / "tests" / "tools"
        nested.mkdir(parents=True)
        result = self.run_script("run", "printf", "%s", "$(do not execute)",
                                 "a b", cwd=nested, CC="clang", CTEST_PARALLEL_LEVEL="4",
                                 PRIVATE_TOKEN="do-not-forward", FAKE_RUN_STATUS="37")
        self.assertEqual(result.returncode, 37, result.stderr)
        call = self.calls()[-1]
        # Command and argument boundaries survive the shell and Docker wrapper.
        self.assertEqual(call[-4:], ["printf", "%s", "$(do not execute)", "a b"])
        self.assertIn("/__w/ayni/ayni/tests/tools", call)
        self.assertIn("CC", call)
        self.assertIn("CTEST_PARALLEL_LEVEL", call)
        self.assertNotIn("PRIVATE_TOKEN", call)
        self.assertIn("--user", call)
        self.assertIn("--memory=3g", call)
        self.assertIn("--cpus=4", call)
        self.assertIn("--pull=never", call)
        self.assertIn("--init", call)
        self.assertFalse(any(c[0] == "create" for c in self.calls()))

    def test_mmap_read_has_no_checkout_or_cache_mounts(self):
        result = self.run_script("mmap-bits")
        self.assertEqual(result.returncode, 0, result.stderr)
        call = self.calls()[-1]
        self.assertEqual(call[-2:], ["cat", "/proc/sys/vm/mmap_rnd_bits"])
        self.assertNotIn("--user", call)
        self.assertNotIn("--mount", call)
        self.assertNotIn("--volume", call)

    def test_run_rejects_working_directory_outside_checkout(self):
        result = self.run_script("run", "true", cwd=self.root)
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertFalse(self.log.exists())


if __name__ == "__main__":
    unittest.main()
