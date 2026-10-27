"""Check normal measurement output without imposing machine-dependent timings."""
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix="ayni-measure-") as directory:
    output = Path(directory) / "result.json"
    subprocess.run([sys.argv[1], "--small", str(output)], check=True, timeout=50)
    result = json.loads(output.read_text(encoding="utf-8"))
    assert result["schema"] == 1 and result["profile"] == "small"
    assert result["close_status"] == 0
    assert result["goals"] == 16 and result["tasks"] == 8
    assert result["grants"] == result["memories"] == 8
    assert result["calls"] == 0 and result["full_work_items"] > 0
    assert 0 < result["single_encoded_bytes"] <= result["batch_bound_bytes"]
    assert 0 < result["bulk_encoded_bytes"] <= result["batch_bound_bytes"]
    assert result["storage_min_bytes"] > result["batch_bound_bytes"]
    for name in ("submit", "encoding_single", "encoding_bulk", "step_population_and_drain", "step_full_work", "step_quiet"):
        samples = result[name]
        assert samples["count"] > 0 and 0 < samples["p99_ns"] <= samples["max_ns"]
    assert result["step_max_items"] == 1 and result["step_max_time_us"] == 1000
    assert result["accepted_buffer_min_bytes"] >= max(
        result["storage_min_bytes"], result["snapshot_record_bound_bytes"]
    )
    revision = subprocess.run(
        ["git", "rev-parse", "HEAD"], cwd=Path(__file__).resolve().parents[2],
        check=True, text=True, capture_output=True,
    ).stdout.strip()
    assert result["source_revision"] == revision and result["source_dirty"] in (0, 1)
    metadata = json.loads(subprocess.run(
        [sys.argv[1], "--build-info"], check=True, text=True, capture_output=True,
    ).stdout)
    for field in ("source_revision", "source_dirty", "build_type", "sanitizer", "compiler"):
        assert metadata[field] == result[field]

# The C99 query and C++ workload share one maximum configuration. Arena byte
# counts depend on ABI/platform/build; compare profiles instead of fixing them.
arena_query = subprocess.run([sys.argv[2]], check=True, text=True, capture_output=True)
rows = re.findall(r"status=(\d+) bytes=(\d+) alignment=(\d+)", arena_query.stdout)
assert len(rows) == 5 and all(status == "0" for status, _, _ in rows)
default, minimum, calls, execution, maximum = (int(row[1]) for row in rows)
assert 0 < minimum < default < calls < execution < maximum
