"""Choose extra CI configurations from the PR merge diff; ordinary code uses ASan."""
import argparse
import json
from pathlib import PurePosixPath
import subprocess


def select_checks(event, paths, full=False):
    """Keep periodic coverage broad while limiting each PR to affected platforms."""
    if event not in {"pull_request", "push", "schedule", "workflow_dispatch"}:
        raise ValueError(f"Unsupported CI event: {event}")
    checks = dict.fromkeys(("asan", "release", "clang", "tsan", "esp_idf"), False)
    if event == "push":
        return checks
    if full or event in {"schedule", "workflow_dispatch"}:
        return dict.fromkeys(checks, True)

    for path in paths:
        if path.endswith(".md"):
            continue
        checks["asan"] = True
        # Build/toolchain changes can affect every supported compiler and target.
        if (PurePosixPath(path).name == "CMakeLists.txt"
                or path == "CMakePresets.json"
                or path.startswith(("cmake/", "tools/ci/", ".github/workflows/"))):
            return dict.fromkeys(checks, True)
        if path.startswith("include/ayni/"):
            checks["clang"] = checks["esp_idf"] = True
        if path.startswith(("hosts/esp_idf/", "runtime/esp_idf_node/",
                            "targets/esp32_assistant/", "drivers/esp32/")):
            checks["esp_idf"] = True
        if (path.startswith(("core/kernel/ingress/", "core/kernel/completion/",
                             "hosts/native/", "hosts/sim/", "runtime/native_node/"))
                or path in {"core/kernel/ingress_lock.h",
                            "tests/unit/event_queue_test.cpp",
                            "tests/unit/kernel_event_test.cpp",
                            "tests/unit/completion_pool_test.cpp",
                            "tests/unit/ingress_test_peer.h",
                            "tests/unit/native_host_test.cpp"}):
            checks["tsan"] = True
    return checks


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("event")
    parser.add_argument("--full", action="store_true")
    args = parser.parse_args()
    paths = []
    if args.event == "pull_request":
        # Checkout supplies the synthetic merge and its base parent. Disable rename
        # detection so a move out of a sensitive directory still selects its checks.
        result = subprocess.run(
            ["git", "diff", "--no-renames", "--name-only", "-z", "HEAD^1", "HEAD"],
            check=True, stdout=subprocess.PIPE)
        paths = result.stdout.decode("utf-8", errors="surrogateescape").split("\0")
        paths = [path for path in paths if path]
    checks = select_checks(args.event, paths, args.full)
    for name, enabled in checks.items():
        print(f"{name}={json.dumps(enabled)}")


if __name__ == "__main__":
    main()
