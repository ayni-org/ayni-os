#!/usr/bin/env bash
# Lightweight repository checks and optional ASan share one cached container.
set -euo pipefail
case "${1:-}" in
    true|false) run_asan=$1 ;;
    *) echo "Expected the ASan selection (true or false)" >&2; exit 2 ;;
esac
python3 -m unittest discover -s tests/tools -p 'test_ci_*.py' -v
python3 tools/check_deps.py
python3 tools/check_contracts.py
python3 tools/check_readability.py
git ls-files -z '*.c' '*.cpp' '*.h' '*.hpp' | xargs -0 clang-format-18 --dry-run --Werror
git diff --check "$(git hash-object -t tree /dev/null)" HEAD
if [ "$run_asan" = true ]; then
    # CTest also runs checker unit tests, ABI probes and all existing regressions.
    bash tools/ci/native.sh native-asan
else
    # Markdown/main checks have no native build to run these through CTest.
    python3 -m unittest discover -s tests/tools -p 'test_check_*.py' -v
fi
