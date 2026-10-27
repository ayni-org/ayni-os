#!/usr/bin/env python3
"""Validate P6's closed normal-fixture profile and compile it to C++ data (ADR-0068)."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import subprocess
import sys

import check_contracts

SCHEMA = 1
ACTION_MAX = 128
CASE_MAX = 32
LABEL = re.compile(r"[a-z][a-z0-9_]{0,31}\Z")
CASE_ID = re.compile(r"E[1-4]-N[1-9][0-9]*\Z")
METRICS = {
    "task_state", "task_steps", "model_calls", "model_limit", "hold_reason", "hold_status", "goal_group_paused", "goal_state",
    "goal_count", "call_count", "model_a_executions", "model_b_executions", "presentations", "increments", "health_executions",
    "max_model_holders", "max_camera_holders", "local_steps", "subscription_stops",
    "barrier_checks", "preserved_calls",
}
GOALS = {"model", "repeat_model", "action", "local", "memory_read", "checkpoint", "composite",
         "reminder", "device_check"}
STATES = {
    "task_state": {name: f"AYNI_TASK_{name}" for name in
                   ("READY", "RUNNING", "PAUSED", "WAITING", "COMPLETED", "CANCELLED")},
    "goal_state": {name: f"AYNI_GOAL_{name}" for name in
                   ("ACTIVE", "PAUSED", "SATISFIED", "CANCELLED")},
    "hold_status": {"DENIED": "AYNI_DENIED"},
    "hold_reason": {"MEMORY_SOURCE": "AYNI_REASON_MEMORY_SOURCE",
                    "TASK_MODEL_CALL_LIMIT": "AYNI_REASON_TASK_MODEL_CALL_LIMIT"},
}


class FixtureError(ValueError):
    pass


def keys(value, required, optional=()):
    if not isinstance(value, dict) or set(value) != set(required) | (set(value) & set(optional)):
        raise FixtureError(f"expected fields {sorted(required)}; optional {sorted(optional)}")


def number(value, maximum, minimum=0, *, decimal=False):
    if decimal:
        if not isinstance(value, str) or not re.fullmatch(r"0|[1-9][0-9]*", value):
            raise FixtureError("64-bit values must be canonical decimal strings")
        value = int(value)
    if type(value) is not int or not minimum <= value <= maximum:
        raise FixtureError(f"integer outside [{minimum}, {maximum}]")
    return value


def string(value, limit=384):
    if not isinstance(value, str) or not value or "\0" in value or len(value.encode()) > limit:
        raise FixtureError(f"expected nonempty UTF-8 string of at most {limit} bytes")
    return value


def quote(value):
    # Encode UTF-8 bytes, including supplementary characters and controls, as C++
    # literals. JSON's surrogate-pair escapes are not legal C++ universal characters.
    escaped = []
    for byte in value.encode("utf-8"):
        if byte in (ord('"'), ord('\\')):
            escaped.append("\\" + chr(byte))
        elif 32 <= byte < 127:
            escaped.append(chr(byte))
        else:
            escaped.append(f"\\{byte:03o}")
    return '"' + "".join(escaped) + '"'


def reference(value, names, kind):
    if value not in names or names[value] != kind:
        raise FixtureError(f"unknown {kind} label {value!r}")
    return quote(value)


def expectation(item, names):
    keys(item, {"metric", "value"}, {"target", "compare"})
    metric = item["metric"]
    if metric not in METRICS:
        raise FixtureError(f"unsupported metric {metric!r}")
    target = item.get("target", "")
    if metric in {"task_state", "task_steps", "model_calls", "model_limit", "hold_reason", "hold_status", "goal_group_paused",
                  "goal_state", "local_steps"}:
        reference(target, names, "goal")
    elif target:
        raise FixtureError("whole-fixture metric cannot name a target")
    comparison = item.get("compare", "equal")
    if comparison not in {"equal", "at_most", "at_least"}:
        raise FixtureError("unsupported comparison")
    value = item["value"]
    if metric in STATES:
        if value not in STATES[metric]:
            raise FixtureError(f"unknown state/reason {value!r}")
        scalar = STATES[metric][value]
    else:
        scalar = f"INT64_C({number(value, 2**63 - 1, decimal=True)})"
    return f"Expect{{Metric::{metric}, {quote(target)}, Compare::{comparison}, {scalar}}}"


def action(item, names, goal_kinds):
    if not isinstance(item, dict) or "op" not in item:
        raise FixtureError("action needs an op")
    op = item["op"]

    def shape(required, optional=()):
        keys(item, {"op", *required}, optional)

    def agent():
        return number(item["agent"], 2, 1)

    def name(kind):
        value = item["name"]
        if not isinstance(value, str) or not LABEL.fullmatch(value) or value in names:
            raise FixtureError("new object needs a unique case-local label")
        names[value] = kind
        return quote(value)

    if op == "grant":
        shape({"name", "agent", "capability"})
        capability = item["capability"]
        if capability not in {"model", "increment", "notify", "observe", "health"}:
            raise FixtureError("unknown capability alias")
        return f"Grant{{{name('grant')}, {agent()}, {quote(capability)}}}"
    if op == "save_memory":
        shape({"name", "agent", "body"})
        return f"SaveMemory{{{name('memory')}, {agent()}, {quote(string(item['body']))}}}"
    if op == "share_memory":
        shape({"name", "memory", "agent"})
        memory = reference(item["memory"], names, "memory")
        return f"ShareMemory{{{name('grant')}, {memory}, {agent()}}}"
    if op == "revoke":
        shape({"grant"})
        return f"Revoke{{{reference(item['grant'], names, 'grant')}}}"
    if op == "goal":
        shape({"name", "agent", "kind"}, {"memory", "pause_group", "calendar_s", "body"})
        kind = item["kind"]
        if kind not in GOALS:
            raise FixtureError("unknown goal kind")
        memory = item.get("memory", "")
        if kind == "memory_read":
            reference(memory, names, "memory")
        elif memory:
            raise FixtureError("only memory_read takes a memory label")
        group = number(item.get("pause_group", 0), 8)
        if kind == "device_check" and group != 1:
            raise FixtureError("assistant device checks belong to group 1")
        calendar = number(item.get("calendar_s", "0"), 253402300799, decimal=True)
        body = item.get("body", "")
        if kind == "reminder":
            string(body)
            if group:
                raise FixtureError("reminders stay outside the autonomy group")
        elif body or calendar:
            raise FixtureError("calendar/body are reminder operands")
        goal_enum = "AssistantGoal" if kind in {"reminder", "device_check"} else "Goal"
        label = name("goal")
        goal_kinds[item["name"]] = kind
        return (f"CreateGoal{{{label}, {agent()}, {goal_enum}::{kind}, {quote(memory)}, "
                f"{group}, INT64_C({calendar}), {quote(body)}}}")
    if op == "steps":
        shape({"count"})
        return f"Steps{{{number(item['count'], 100000, 1)}}}"
    if op == "advance_clock":
        shape({"microseconds"})
        return f"Advance{{UINT64_C({number(item['microseconds'], 10**12, decimal=True)})}}"
    if op == "set_wall_clock":
        shape({"seconds", "trust"})
        if item["trust"] not in {"TRUSTED", "UNTRUSTED", "UNAVAILABLE"}:
            raise FixtureError("unknown UTC trust")
        return (f"WallClock{{INT64_C({number(item['seconds'], 253402300799, decimal=True)}), "
                f"AYNI_UTC_{item['trust']}}}")
    if op in {"wait", "assert"}:
        shape({"condition"})
        cls = "Wait" if op == "wait" else "Check"
        return f"{cls}{{{expectation(item['condition'], names)}}}"
    if op in {"pause_group", "resume_group"}:
        shape({"group"})
        cls = "PauseGroup" if op == "pause_group" else "ResumeGroup"
        return f"{cls}{{{number(item['group'], 8, 1)}}}"
    if op in {"pause_goal", "resume_goal", "local_input", "retry"}:
        shape({"goal"})
        if op == "local_input" and goal_kinds.get(item["goal"]) != "local":
            raise FixtureError("local_input needs a local Goal")
        cls = {"pause_goal": "PauseGoal", "resume_goal": "ResumeGoal",
               "local_input": "LocalInput", "retry": "Retry"}[op]
        return f"{cls}{{{reference(item['goal'], names, 'goal')}}}"
    if op == "reopen":
        shape({"provider", "strategy"})
        if item["provider"] not in {"a", "b"} or item["strategy"] not in {"v1", "v2"}:
            raise FixtureError("unknown clean-reopen configuration")
        return f"Reopen{{Provider::{item['provider']}, Strategy::{item['strategy']}}}"
    raise FixtureError(f"unsupported normal action {op!r}; fault/recovery actions remain paused")


def no_duplicates(pairs):
    out = {}
    for key, value in pairs:
        if key in out:
            raise FixtureError(f"duplicate JSON key {key!r}")
        out[key] = value
    return out


def reject_constant(_):
    raise FixtureError("nonfinite/floating JSON is outside the normal profile")


def check_nesting(value):
    pending = [(value, 1)]
    while pending:
        item, depth = pending.pop()
        if depth > 8:
            raise FixtureError("JSON nesting exceeds 8")
        children = item.values() if isinstance(item, dict) else item if isinstance(item, list) else ()
        for child in children:
            if isinstance(child, (dict, list)):
                pending.append((child, depth + 1))


def load_case(path, clauses, steps):
    if path.stat().st_size > 65536:
        raise FixtureError("fixture exceeds 64 KiB")
    try:
        value = json.loads(path.read_text(), object_pairs_hook=no_duplicates,
                           parse_constant=reject_constant, parse_float=reject_constant)
    except RecursionError as error:
        raise FixtureError("JSON nesting exceeds 8") from error
    check_nesting(value)
    keys(value, {"fixture_schema", "profile", "case_id", "experiment_step", "clauses",
                 "description", "seed", "config", "script", "expect"})
    if type(value["fixture_schema"]) is not int or value["fixture_schema"] != SCHEMA or value["profile"] != "m1-normal":
        raise FixtureError("expected fixture_schema 1 / profile m1-normal")
    case_id = value["case_id"]
    if not isinstance(case_id, str) or not CASE_ID.fullmatch(case_id) or path.stem != case_id:
        raise FixtureError("normal case ID must match E1-N1 form and filename")
    if value["experiment_step"] not in steps or not value["experiment_step"].startswith(case_id[:2]):
        raise FixtureError("experiment_step does not name a matching plan step")
    if not isinstance(value["clauses"], list) or not value["clauses"] or any(c not in clauses for c in value["clauses"]):
        raise FixtureError("clauses must name defined contracts")
    string(value["description"])
    if value["seed"] != "0":
        raise FixtureError("normal profile uses explicit ordering, seed must be 0")
    cfg = value["config"]
    keys(cfg, {"tick_us", "model_delay_us", "step_units", "max_steps", "model_hard_limit"})
    config = (f"Config{{UINT64_C({number(cfg['tick_us'], 1000000, 1, decimal=True)}), "
              f"UINT64_C({number(cfg['model_delay_us'], 10000000, 1, decimal=True)}), "
              f"{number(cfg['step_units'], 1024, 1)}, {number(cfg['max_steps'], 100000, 1)}, "
              f"{number(cfg['model_hard_limit'], 64, 1)}}}")
    for field in ("script", "expect"):
        if not isinstance(value[field], list) or not 1 <= len(value[field]) <= ACTION_MAX:
            raise FixtureError(f"{field} needs 1..{ACTION_MAX} entries")
    names = {}
    goal_kinds = {}
    actions = [action(item, names, goal_kinds) for item in value["script"]]
    expects = [expectation(item, names) for item in value["expect"]]
    return value, config, actions, expects


def generate(root, paths):
    problems = []
    documents = check_contracts.parse_documents(root, problems, set())
    if problems:
        raise FixtureError("contract source is malformed")
    clauses = {c.id for d in documents for c in d.clauses}
    steps = set(check_contracts.experiment_steps(root))
    if not 1 <= len(paths) <= CASE_MAX:
        raise FixtureError(f"expected 1..{CASE_MAX} fixtures")
    records, definitions, ids = [], [], []
    for index, path in enumerate(sorted(paths)):
        try:
            value, config, actions, expects = load_case(path, clauses, steps)
        except (ValueError, TypeError, KeyError, UnicodeError) as error:
            raise FixtureError(f"{path}: {error}") from error
        if value["case_id"] in ids:
            raise FixtureError("duplicate case ID")
        ids.append(value["case_id"])
        definitions.extend([f"const Action actions_{index}[] = {{\n    " + ",\n    ".join(actions) + "\n};",
                            f"const Expect expect_{index}[] = {{\n    " + ",\n    ".join(expects) + "\n};"])
        strings = [value["case_id"], value["experiment_step"], value["description"],
                   ",".join(value["clauses"]), path.relative_to(root).as_posix()]
        records.append("{" + ", ".join(map(quote, strings)) + f", {config}, actions_{index}, "
                       f"{len(actions)}, expect_{index}, {len(expects)}}}")
    revision = subprocess.run(["git", "rev-parse", "HEAD"], cwd=root, check=True,
                              text=True, capture_output=True).stdout.strip()
    dirty = subprocess.run(["git", "status", "--porcelain"], cwd=root, check=True,
                           text=True, capture_output=True).stdout
    if dirty:
        revision += "-dirty"
    source = ('// Generated by generate_normal_fixtures.py; do not edit.\n'
              '#include <ayni/runtime/sim_node/fixtures.h>\n'
              'namespace ayni::runtime::fixtures { namespace {\n' + "\n".join(definitions) +
              '\nconst Case table[] = {\n    ' + ",\n    ".join(records) + '\n};\n}\n'
              'Cases cases() { return {table, sizeof(table) / sizeof(table[0])}; }\n'
              f'const char *source_revision() {{ return {quote(revision)}; }}\n' + '}\n')
    return ids, source


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parent.parent)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--list", action="store_true")
    args = parser.parse_args(argv)
    try:
        ids, source = generate(args.root, list((args.root / "contracts/fixtures/normal").glob("*.json")))
        if args.list:
            print(";".join(ids))
        if args.output and (not args.output.exists() or args.output.read_text() != source):
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(source)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"generate_normal_fixtures: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
