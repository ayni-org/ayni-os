"""Fixed source dependency of the common normal profile; no root deployment or test fixtures."""
from contextlib import contextmanager
from pathlib import Path
import shutil


@contextmanager
def source_sdk(source: Path, destination: Path, host: str):
    if host not in {"native", "esp_idf"}:
        raise ValueError("Unsupported independent Host")
    for relative in ("cmake", "include", "core", "strategies/builtin", "providers/software",
                     "providers/fixture_model", "providers/mock_execution", "control/permit_gate",
                     "applications/software_workflow", "hosts/storage", f"hosts/{host}",
                     "runtime/portable", "runtime/conversation", "runtime/common/include"):
        shutil.copytree(source / relative, destination / relative)
    (destination / "tools").mkdir()
    shutil.copy2(source / "tools/architecture.json", destination / "tools/architecture.json")
    for path in destination.rglob("*"):
        path.chmod(0o555 if path.is_dir() else 0o444)
    destination.chmod(0o555)
    try:
        yield destination
    finally:
        # Build output belongs to the consumer. Restore only directory permissions
        # so the enclosing temporary tree can be removed, never rewrite SDK content.
        for path in destination.rglob("*"):
            if path.is_dir():
                path.chmod(0o755)
        destination.chmod(0o755)
