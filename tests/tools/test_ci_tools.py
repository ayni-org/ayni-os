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
        nproc = self.bin / "nproc"
        nproc.write_text('#!/usr/bin/env bash
'
                         'printf "%s\n" "${FAKE_HOST_CPUS:-32}"
')
        nproc.chmod(0o755)
        self.log = self.root / "calls.jsonl"
        docker = self.bin / "docker"
        docker.write_text('''#!/usr/bin/env python3
import json, os, pathlib, subprocess, sys
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
elif args[0] == "run" and "FAKE_CACHE_ROOT" in os.environ and "-c" in args:
    # Run the cache adoption script on local directories, as another user if asked.
    root = os.environ["FAKE_CACHE_ROOT"]
    script = args[args.index("-c") + 1]
    script = script.replace("/ccache", root + "/ccache").replace("/downloads", root + "/downloads")
    identity = os.environ.get("FAKE_IDENTITY", args[-1])
    sys.exit(subprocess.call(["bash", "-eu", "-c", script, "bash", identity]))
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
        self.assertIn("--cpus=12", call)
        self.assertIn("--pull=never", call)
        self.assertIn("--init", call)
        self.assertFalse(any(c[0] == "create" for c in self.calls()))

    def test_four_cpu_host_caps_bootstrap_and_execution(self):
        prepare = self.run_script("prepare", FAKE_HOST_CPUS="4")
        self.assertEqual(prepare.returncode, 0, prepare.stderr)
        command = self.run_script("run", "true", FAKE_HOST_CPUS="4")
        self.assertEqual(command.returncode, 0, command.stderr)
        capped = [call for call in self.calls() if call[0] in {"create", "run"}]
        self.assertEqual([call[0] for call in capped], ["create", "run", "run"])
        for call in capped:
            self.assertIn("--cpus=4", call)
            self.assertNotIn("--cpus=12", call)

    def test_prepare_adopts_root_caches_but_not_those_of_another_user(self):
        # Two runners share the Docker daemon and its caches: taking a cache
        # from the other runner's user would break that runner's running job.
        caches = self.root / "caches"
        for name in ("ccache", "downloads"):
            (caches / name).mkdir(parents=True)
        own = f"{os.getuid()}:{os.getgid()}"
        result = self.run_script("prepare", FAKE_CACHE_ROOT=str(caches), FAKE_IDENTITY=own)
        self.assertEqual(result.returncode, 0, result.stderr)
        if os.getuid() == 0:
            # A new volume belongs to root; the runners' user adopts it once.
            result = self.run_script("prepare", FAKE_CACHE_ROOT=str(caches),
                                     FAKE_IDENTITY="4242:4242")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual((caches / "ccache").stat().st_uid, 4242)
        result = self.run_script("prepare", FAKE_CACHE_ROOT=str(caches), FAKE_IDENTITY="4343:4343")
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("run every runner on this Docker daemon as one user", result.stderr)
        self.assertNotEqual((caches / "ccache").stat().st_uid, 4343)

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
