#!/bin/bash
set -euo pipefail
project_dir="$(cd "$(dirname "$0")" && pwd)"
export AYNI_CHAT_PYTHON="${AYNI_CHAT_PYTHON:-/Users/danny/Documents/Codex/2026-10-20/task/.tools/espressif/python_env/idf6.0_py3.14_env/bin/python}"
exec "$project_dir/build/AyniUSBChat"
