#!/usr/bin/env bash
# Explicit owner grants, two Agents, pause-preserved reminder and orderly close.
# Mock only; an unsynchronized Native UTC retains the reminder until time is trusted.
set -euo pipefail
ayni_demo_binary=${1:-build/native/targets/native_cli/ayni}
ayni_demo_directory=$(mktemp -d)
trap 'rm -rf "$ayni_demo_directory"' EXIT
printf 'provider=mock\n' > "$ayni_demo_directory/config"
"$ayni_demo_binary" --config "$ayni_demo_directory/config" --data-dir "$ayni_demo_directory/state" <<'COMMANDS'
/grant a present
/wait
/grant a health
/wait
/grant a notify
/wait
/grant b model
/wait
/grant b summary
/wait
/grant b present
/wait
/grant b observe
/wait
@a check
/wait
你好，请用中文简短回答
/wait
/check a 300
/wait
/observe b
/wait
/pause-assistant
/wait
/status
/remind once 1970-01-02T00:00 UTC0 演示提醒（可信校时后补发一次）
/wait
/notifications
/resume-assistant
/wait
/conversation
/diagnostics
/quit
COMMANDS
