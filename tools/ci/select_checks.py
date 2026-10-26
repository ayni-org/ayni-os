"""Use ASan for code PRs; reserve other configurations for periodic/explicit runs."""
import argparse
import json
import subprocess


def select_checks(event, paths, full=False):
    """Select one native suite for a normal PR, regardless of which code changed."""
    if event not in {"pull_request", "push", "schedule", "workflow_dispatch"}:
        raise ValueError(f"Unsupported CI event: {event}")
    checks = dict.fromkeys(("asan", "release", "clang", "tsan", "esp_idf"), False)
    if event == "push":
        return checks
    if full or event in {"schedule", "workflow_dispatch"}:
        return dict.fromkeys(checks, True)

    checks["asan"] = any(not path.endswith(".md") for path in paths)
    return checks


# The preset that runs each extra native configuration, in matrix order.
NATIVE_PRESETS = {"release": "native-release", "clang": "native-debug", "tsan": "native-tsan"}


def native_presets(checks):
    """The extra native presets selected, one matrix job each."""
    return [preset for name, preset in NATIVE_PRESETS.items() if checks[name]]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("event")
    parser.add_argument("--full", action="store_true")
    args = parser.parse_args()
    paths = []
    if args.event == "pull_request":
        # Checkout supplies the synthetic merge and its base parent. Disable rename
        # detection so moving code to a Markdown path still selects the code suite.
        result = subprocess.run(
            ["git", "diff", "--no-renames", "--name-only", "-z", "HEAD^1", "HEAD"],
            check=True, stdout=subprocess.PIPE)
        paths = result.stdout.decode("utf-8", errors="surrogateescape").split("\0")
        paths = [path for path in paths if path]
    checks = select_checks(args.event, paths, args.full)
    for name, enabled in checks.items():
        print(f"{name}={json.dumps(enabled)}")
    print(f"native_presets={json.dumps(native_presets(checks), separators=(',', ':'))}")


if __name__ == "__main__":
    main()
