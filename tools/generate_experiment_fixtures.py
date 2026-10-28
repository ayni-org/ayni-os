#!/usr/bin/env python3
"""Compile the bounded full-experiment profile; never interpret JSON in Core."""
from __future__ import annotations
import argparse
import json
from pathlib import Path
import re
import subprocess
import sys

import check_contracts
from generate_normal_fixtures import (FixtureError, keys, number, quote, string,
                                      no_duplicates, reject_constant, check_nesting)

CASE_ID = re.compile(r"(?:HARNESS|E[1-4])\.(?:S[1-9][0-9]*|F(?:0|[1-9][0-9]*))\.[a-z][a-z0-9_]*\Z")
METRICS = {"executions", "pending_calls", "pending_storage", "goal_count", "call_count",
           "call_phase", "call_outcome", "call_effect", "lifecycle", "writer_epoch", "faults_applied"}
VALUES = {
    "call_phase": {name: "AYNI_CALL_" + name for name in
                   ("QUEUED", "PREPARED", "DISPATCHING", "IN_PROGRESS", "FINISHED", "RESULT_UNKNOWN")},
    "call_outcome": {name: "AYNI_CALL_" + name for name in
                     ("COMPLETED", "FAILED", "CANCELLED", "ABANDONED")},
    "call_effect": {name: "AYNI_CALL_EFFECT_" + name for name in
                    ("NONE", "PARTIAL", "COMPLETE", "UNKNOWN")},
    "lifecycle": {name: "AYNI_LIFECYCLE_" + name for name in
                  ("RUNNING", "FENCED", "RESTRICTED", "RECOVERING", "STOPPED")},
}
PORTS = {"append", "read_prefix", "read_log", "fence", "write_snapshot", "read_snapshot", "trim_prefix"}
FAULTS = {"reject", "fail", "unknown_lost", "unknown_persisted", "short_write",
          "unknown_deferred", "accept_unknown", "unavailable"}


def metric(item):
    name = item["metric"]
    if name not in METRICS:
        raise FixtureError("unknown metric")
    value = item["value"]
    if name in VALUES:
        if value not in VALUES[name]:
            raise FixtureError("unknown metric state")
        expected = VALUES[name][value]
    else:
        expected = f"UINT64_C({number(value, 2**63 - 1, decimal=True)})"
    return name, expected


def action(item):
    if not isinstance(item, dict) or "op" not in item:
        raise FixtureError("action needs op")
    op = item["op"]
    if op in {"start", "execute", "grant", "request_query"}:
        keys(item, {"op"})
        return {"start": "w.start()", "grant": "w.grant()", "request_query": "w.request_query()",
                "execute": 'w.evidence.status("execute effect", w.service.execute(w.node.kernel()))'}[op]
    if op == "answer_query":
        keys(item, {"op", "result"})
        if item["result"] not in {"FOUND", "NOT_ACCEPTED", "UNKNOWN"}:
            raise FixtureError("unknown query result")
        return ('w.evidence.status("query answer", w.service.answer_query('
                f'AYNI_QUERY_RESULT_{item["result"]}))')
    if op == "create":
        keys(item, {"op", "type", "command"})
        if item["type"] not in {"folder", "action"}:
            raise FixtureError("unknown goal type")
        command = number(item["command"], 2**63 - 1, 1, decimal=True)
        return f'w.create({quote(item["type"])}, UINT64_C({command}))'
    if op == "steps":
        keys(item, {"op", "count"}, {"storage"})
        count = number(item["count"], 256, 1)
        mode = item.get("storage", "complete")
        if mode not in {"complete", "held"}:
            raise FixtureError("unknown storage mode")
        return f"w.storage = World::Storage::{mode}; w.settle({count})"
    if op in {"expect", "until"}:
        keys(item, {"op", "metric", "value"}, {"bound"} if op == "until" else ())
        name, expected = metric(item)
        if op == "expect":
            return f'w.evidence.equal({quote(name)}, w.metric(Metric::{name}), {expected})'
        bound = number(item.get("bound", 256), 1000, 1)
        return (f'w.until({quote("wait " + name)}, [&] {{ return '
                f'w.metric(Metric::{name}) == {expected}; }}, {bound})')
    if op == "fault":
        keys(item, {"op", "port", "fault"})
        port, fault = item["port"], item["fault"]
        if port not in PORTS or fault not in FAULTS:
            raise FixtureError("unsupported storage fault")
        return (f'w.node.simulation()->inject_fault(hosts::sim::StoragePort::{port}, '
                f'hosts::sim::StorageFault::{fault})')
    if op == "crash":
        keys(item, {"op", "append"})
        disposition = item["append"]
        if disposition not in {"lost", "stored", "torn"}:
            raise FixtureError("unknown cut disposition")
        return f"w.crash(hosts::sim::CutAppend::{disposition})"
    if op == "receipt":
        keys(item, {"op", "outcome", "effect"})
        outcome, effect = item["outcome"], item["effect"]
        if outcome not in VALUES["call_outcome"] or effect not in VALUES["call_effect"]:
            raise FixtureError("unknown receipt")
        return ('w.evidence.status("provider receipt", w.service.receipt('
                f'{VALUES["call_outcome"][outcome]}, {VALUES["call_effect"][effect]}))')
    raise FixtureError(f"unsupported action {op!r}")


def load_case(path, clauses, steps):
    if path.stat().st_size > 65536:
        raise FixtureError("fixture exceeds 64 KiB")
    try:
        item = json.loads(path.read_text(), object_pairs_hook=no_duplicates,
                          parse_constant=reject_constant, parse_float=reject_constant)
    except RecursionError as error:
        raise FixtureError("JSON nesting exceeds 8") from error
    check_nesting(item)
    keys(item, {"fixture_schema", "profile", "case_id", "experiment_step", "clauses", "script"})
    if type(item["fixture_schema"]) is not int or item["fixture_schema"] != 1 or item["profile"] != "m1-experiment":
        raise FixtureError("unsupported schema/profile")
    case_id = string(item["case_id"], 128)
    if not CASE_ID.fullmatch(case_id) or path.stem != case_id:
        raise FixtureError("case id must match file and full-experiment grammar")
    experiment, point, _ = case_id.split(".")
    step = (f"{experiment} 步骤 {point[1:]}" if experiment != "HARNESS" and point.startswith("S")
            else f"{experiment} {point}")
    if experiment != "HARNESS" and step not in steps:
        raise FixtureError("experiment step is absent from the plan")
    if item["experiment_step"] != step:
        raise FixtureError("experiment step does not match id")
    if not isinstance(item["clauses"], list) or not item["clauses"] or any(c not in clauses for c in item["clauses"]):
        raise FixtureError("unknown/empty contract clauses")
    script = item["script"]
    if not isinstance(script, list) or not 1 <= len(script) <= 128:
        raise FixtureError("expected 1..128 actions")
    if not isinstance(script[-1], dict) or script[-1].get("op") != "expect":
        raise FixtureError("case needs explicit final evidence")
    return item, case_id, step, script


def compile_cases(root):
    problems = []
    documents = check_contracts.parse_documents(root, problems, set())
    if problems:
        raise FixtureError("contract source is malformed")
    clauses = {c.id for doc in documents for c in doc.clauses}
    steps = set(check_contracts.experiment_steps(root))
    files = sorted((root / "contracts/fixtures/experiments").glob("*.json"))
    if not files or len(files) > 256:
        raise FixtureError("expected 1..256 cases")
    functions, entries = [], []
    for index, path in enumerate(files):
        item, case_id, step, script = load_case(path, clauses, steps)
        statements = [f"    if (w.evidence.ok()) {{ {action(a)}; }}" for a in script]
        functions.append(f"void scenario_{index}(World &w) {{\n" + "\n".join(statements) + "\n}")
        entries.append("    {" + ", ".join((quote(case_id), quote(step), quote(path.relative_to(root).as_posix()),
                                            quote(",".join(item["clauses"])), f"scenario_{index}")) + "}")
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip()
    if subprocess.check_output(["git", "status", "--porcelain", "--untracked-files=normal"], cwd=root, text=True):
        revision += "+dirty"
    return ('#include "experiment_support.h"\n#include <ayni/runtime/sim_node/experiments.h>\n'
            'namespace ayni::runtime::experiments {\nnamespace {\n' + "\n".join(functions) +
            "\nconst Case table[] = {\n" + ",\n".join(entries) + "\n};\n}\n"
            "Cases cases() { return {table, sizeof(table)/sizeof(table[0])}; }\n"
            f"const char *source_revision() {{ return {quote(revision)}; }}\n}}\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        generated = compile_cases(args.root.resolve())
        args.output.parent.mkdir(parents=True, exist_ok=True)
        if not args.output.exists() or args.output.read_text() != generated:
            args.output.write_text(generated)
    except (FixtureError, ValueError, TypeError, KeyError, OSError, subprocess.CalledProcessError) as error:
        print(f"experiment fixtures: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
