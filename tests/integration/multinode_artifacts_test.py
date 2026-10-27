"""C-FIX-007..010: normal typed execution/replay, including orderly in-flight close."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

TASK_RUNNING = 2
TASK_COMPLETED = 5
EFFECT_COMPLETE = 3

def run(binary, directory):
    process = subprocess.run(
        [binary, "--multinode-normal", "--output", str(directory)],
        text=True, capture_output=True, timeout=40,
    )
    assert process.returncode == 0, process.stdout + process.stderr
    manifest = json.loads((directory / "run.json").read_text(encoding="utf-8"))
    raw_trace = (directory / "trace.jsonl").read_bytes()
    rows = [json.loads(line) for line in raw_trace.splitlines()]
    assert manifest["status"] == manifest["close_status"] == 0
    assert manifest["first_failure"] == "none"
    assert manifest["independent"] == manifest["responsive"] == manifest["inflight_close"] == 1
    assert manifest["effects"] == manifest["barrier_checks"] == 2
    assert manifest["caller_agent"] == "1" and manifest["service_agent"] == "9"
    operations = manifest["operations"]
    assert len(operations) == 2
    assert operations[0]["operation"] != operations[1]["operation"]
    assert operations[0]["service_task"] != operations[1]["service_task"]
    queries = [row["reply"] for row in rows if row["kind"] == "query_reply"]
    assert len(queries) == 4
    for index, operation in enumerate(operations):
        active, done = queries[index * 2:index * 2 + 2]
        assert active["completed"] == 0 and active["task_state"] == TASK_RUNNING
        assert done == operation
        assert done["completed"] == 1 and done["task_state"] == TASK_COMPLETED
        assert done["effect"] == EFFECT_COMPLETE
        assert active["caller_node"] == done["caller_node"] == "a"
        assert active["service_node"] == done["service_node"] == "b"
        for name in ("operation", "caller_task", "caller_instance", "service_goal",
                     "service_task", "service_instance", "acceptance_seq", "durable_seq"):
            assert isinstance(active[name], str) and int(active[name]) > 0
            assert isinstance(done[name], str) and int(done[name]) > 0
        assert active["caller_task"] == done["caller_task"]
        assert active["service_task"] == done["service_task"]
        assert int(active["acceptance_seq"]) <= int(active["durable_seq"])
        assert int(done["acceptance_seq"]) <= int(done["durable_seq"])
    clocks = [row for row in rows if row["kind"] == "clock"]
    assert clocks and len(clocks) <= 8192
    for field in ("transport_us", "a_us", "b_us"):
        assert all(isinstance(row[field], str) for row in clocks)
        times = [int(row[field]) for row in clocks]
        assert times == sorted(times) and len(times) == len(set(times))
    for field in ("a_seq", "b_seq"):
        values = [int(row[field]) for row in clocks]
        assert values == sorted(values)
    assert any(row["kind"] == "call" and row["node"] == "a" for row in rows)
    assert any(row["kind"] == "call" and row["node"] == "b" for row in rows)
    for row in rows:
        assert not ({"body", "text", "credential", "token", "parameters"} & row.keys())
    # A new-directory boundary must not overwrite the evidence already produced.
    again = subprocess.run(
        [binary, "--multinode-normal", "--output", str(directory)],
        text=True, capture_output=True, timeout=10,
    )
    assert again.returncode != 0
    assert (directory / "trace.jsonl").read_bytes() == raw_trace
    return manifest, raw_trace

with tempfile.TemporaryDirectory(prefix="ayni-multinode-") as root:
    first = run(sys.argv[1], Path(root) / "first")
    second = run(sys.argv[1], Path(root) / "second")
    assert first == second, "same normal scenario must produce identical logical artifacts"

