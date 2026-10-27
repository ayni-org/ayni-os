#!/usr/bin/env bash
# Explicit owner grants, one ToolLoop assistant, deterministic stop, pause-preserved reminder and
# orderly close. Mock only; an unsynchronized Native UTC retains the reminder until time is trusted.
set -euo pipefail
ayni_demo_binary=${1:-build/native/targets/native_cli/ayni}
ayni_demo_directory=$(mktemp -d)
trap 'rm -rf "$ayni_demo_directory"' EXIT
printf 'provider=mock\n' > "$ayni_demo_directory/config"
"$ayni_demo_binary" --config "$ayni_demo_directory/config" --data-dir "$ayni_demo_directory/state" <<'COMMANDS'
/grant model
/wait
/grant summary
/wait
/grant schedule
/wait
/grant health
/wait
/grant notify
/wait
/grant observe
/wait
你好，请用中文简短回答
/wait
check
/wait
/check 300
/wait
/observe
/wait
停
/status
/remind once 1970-01-02T00:00 UTC0 演示提醒（可信校时后补发一次）
/wait
/notifications
/resume-assistant
/wait
/status
/goals
/conversation
/diagnostics
/quit
COMMANDS
