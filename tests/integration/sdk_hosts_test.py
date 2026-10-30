"""ADR-0087: one public application, real Native file I/O and normal shutdown."""
import pathlib
import subprocess
import sys
import tempfile


with tempfile.TemporaryDirectory(prefix="ayni-sdk-native-") as directory:
    result = subprocess.run([sys.argv[1], directory], capture_output=True, text=True, timeout=25)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "passed" in result.stdout, result.stdout
    log = pathlib.Path(directory) / "commit_log"
    assert log.is_file() and log.stat().st_size > 0
