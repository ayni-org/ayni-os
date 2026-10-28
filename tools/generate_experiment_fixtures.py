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
           "call_phase", "call_outcome", "call_effect", "lifecycle", "writer_epoch", "faults_applied",
           "submissions", "call_durable", "call_operation", "task_count", "call_query_conclusion",
           "goal_state", "task_state", "kernel_boot", "writer_advanced", "boot_advanced", "durable_kept", "memory_count", "task_steps", "checkpoint_value", "task_occurrence", "model_calls_used", "model_call_limit", "grant_lifecycle", "physical_submissions", "physical_samples", "physical_renewals", "physical_challenges", "held_renewals", "control_phase", "control_deadline", "shared_completed", "replacement_completed"}
VALUES = {
    "grant_lifecycle": {name: "AYNI_GRANT_" + name for name in ("ACTIVE", "REVOKED", "EXPIRED")},
    "control_phase": {name: "AYNI_CONTROL_" + name for name in ("IDLE", "ACTIVE", "STOPPING", "STOPPED")},
    "call_phase": {name: "AYNI_CALL_" + name for name in
                   ("QUEUED", "PREPARED", "DISPATCHING", "IN_PROGRESS", "FINISHED", "RESULT_UNKNOWN")},
    "call_outcome": {name: "AYNI_CALL_" + name for name in
                     ("COMPLETED", "FAILED", "CANCELLED", "ABANDONED")},
    "call_effect": {name: "AYNI_CALL_EFFECT_" + name for name in
                    ("NONE", "PARTIAL", "COMPLETE", "UNKNOWN")},
    "lifecycle": {name: "AYNI_LIFECYCLE_" + name for name in
                  ("RUNNING", "FENCED", "RESTRICTED", "RECOVERING", "STOPPED")},
    "call_query_conclusion": {name: "AYNI_QUERY_RESULT_" + name for name in
                              ("FOUND", "NOT_ACCEPTED", "UNKNOWN")},
    "goal_state": {name: "AYNI_GOAL_" + name for name in ("ACTIVE", "PAUSED", "SATISFIED", "CANCELLED")},
    "task_state": {name: "AYNI_TASK_" + name for name in ("READY", "RUNNING", "PAUSED", "COMPLETED", "CANCELLED")},
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
    if op == "replacement":
        keys(item, {"op", "scenario"})
        scenario = item["scenario"]
        if scenario not in {"baseline_checkpoint", "swap_provider_late_old_result", "unknown_call_not_rebound", "new_call_uses_new_binding", "completed_operation_returns_old", "swap_strategy_at_checkpoint", "incompatible_private_checkpoint", "unknown_strategy_version"}:
            raise FixtureError("unsupported replacement scenario")
        return ('w.evidence.equal("replacement deployment required", w.replacement != nullptr, 1); '
                f'if (w.evidence.ok()) w.replacement->run(ReplacementScenario::{scenario})')
    if op == "shared":
        keys(item, {"op", "scenario"})
        scenario = item["scenario"]
        if scenario not in {"model_slot_contention", "local_rule_progresses", "private_read_denied", "explicit_share_then_revoke", "profile_scoped_to_agent", "voice_cannot_grant", "model_limit_two", "restart_keeps_model_calls", "exclusive_queue_shared_parallel", "resource_group_all_or_nothing", "timeout_cancel_releases", "close_stops_generation", "renewal_rechecks_grant", "delete_blocks_stale_summary", "user_edit_not_overwritten", "cleanup_resumes_after_restart", "revoke_blocks_stale_summary", "pause_keeps_reminder", "presentation_success_completes", "presentation_failed_pending", "presentation_unknown_pending", "duplicate_receipt_once", "receipt_lost_reconciled", "quiet_hours_deferred", "estop_keeps_reminder", "completion_slots_when_queue_full", "ordinary_work_expires_with_reason"}:
            raise FixtureError("unsupported shared scenario")
        return ('w.evidence.equal("shared deployment required", w.shared != nullptr, 1); '
                f'if (w.evidence.ok()) w.shared->run(SharedScenario::{scenario})')
    if op == "physical":
        keys(item, {"op", "action"}, {"us", "status", "mode"})
        control = item["action"]
        simple = {"revoke_queued", "revoke_running", "unauthorized", "stop_full_queue", "quarantine", "prepare", "mark_invalidated", "stopped_within_bound", "invalidate_kernel", "invalidate_calibration",
                  "estop", "estop_before_dispatch", "reset_estop", "replay_permit_rejected", "replay_renewal_rejected", "unpermitted_rejected", "power_cycle", "drop_renewal"}
        prefix = 'w.evidence.equal("physical deployment required", w.physical != nullptr, 1); if (w.evidence.ok()) '
        if control in simple:
            keys(item, {"op", "action"})
            return prefix + f'w.physical->{control}()'
        if control in {"at", "elapse"}:
            keys(item, {"op", "action", "us"})
            return prefix + f'w.physical->{control}({number(item["us"], 1000000, 0)})'
        if control == "link":
            keys(item, {"op", "action", "mode"})
            if item["mode"] not in {"connected", "hold_renewals", "disconnected"}:
                raise FixtureError("unsupported physical link")
            return prefix + f'w.physical->executor.set_link(providers::mock_execution::Link::{item["mode"]})'
        if control == "deliver_renewal":
            keys(item, {"op", "action", "status"})
            if item["status"] not in {"OK", "INVALID_STATE", "EXPIRED", "STALE"}:
                raise FixtureError("unsupported renewal verdict")
            return prefix + f'w.physical->deliver_renewal(AYNI_{item["status"]})'
        if control == "evict_challenges":
            keys(item, {"op", "action"})
            return prefix + 'w.evidence.status("evict real nonce window", w.physical->executor.evict_challenges())'
        if control == "not_ready":
            keys(item, {"op", "action"})
            return prefix + 'w.physical->publish(0)'
        raise FixtureError("unsupported physical control")
    if op == "core_now":
        keys(item, {"op", "count"})
        count = number(item["count"], 256, 1)
        return f'for (uint32_t i=0; i < {count} && w.evidence.ok(); ++i) {{ w.step_now(); }}'
    if op in {"start", "finish_recovery"}:
        keys(item, {"op"}, {"expected_lifecycle"})
        expected = item.get("expected_lifecycle", "RUNNING")
        if expected not in {"RUNNING", "RESTRICTED"}:
            raise FixtureError("unsupported recovery outcome")
        return f'w.{op}(AYNI_LIFECYCLE_{expected})'
    if op == "retry":
        keys(item, {"op"}, {"status"})
        status = item.get("status", "OK")
        if status not in {"OK", "NOT_READY"}:
            raise FixtureError("unsupported retry status")
        return f'w.retry(AYNI_{status})'
    if op == "request_query":
        keys(item, {"op"}, {"status"})
        status = item.get("status", "OK")
        if status not in {"OK", "UNSUPPORTED"}:
            raise FixtureError("unsupported query status")
        return f'w.request_query(AYNI_{status})'
    if op == "budget":
        keys(item, {"op", "items"})
        return f'w.items_per_step = {number(item["items"], 64, 1)}'
    if op == "damage":
        keys(item, {"op", "region"})
        if item["region"] not in {"torn_tail", "committed"}:
            raise FixtureError("unsupported damage region")
        return f'w.damage_log(Damage::{item["region"]})'
    if op == "fault_pending":
        keys(item, {"op", "fault"})
        if item["fault"] not in FAULTS - {"reject", "accept_unknown", "unavailable"}:
            raise FixtureError("unsupported pending append fault")
        return f'w.fault_pending(hosts::sim::StorageFault::{item["fault"]})'
    if op in {"repeat_receipt", "post_old_attempt"}:
        keys(item, {"op"})
        return f'w.evidence.status({quote(op)}, w.service.{op}(), AYNI_INVALID_STATE)'
    if op in {"hold_record", "cut_record"}:
        keys(item, {"op", "point"})
        point = item["point"]
        if point not in {"goal_state", "task", "task_state", "task_authority",
                         "prepared_call", "dispatch_call", "terminal_call", "revoked_grant", "memory", "task_checkpoint", "before_marker"}:
            raise FixtureError("unsupported record cut point")
        function = "hold_record_append" if op == "hold_record" else "crash_after_record"
        return f'w.{function}(RecordPoint::{point})'
    if op in {"execute", "grant", "start_recovery", "save_call", "same_call",
              "saved_call_missing", "revoke", "conflict_command", "expire_command", "read_result", "cancel_goal", "complete_storage", "complete_old_writer", "application_policy", "exhaust_storage", "restore_storage"}:
        keys(item, {"op"})
        return {"grant": "w.grant()", "exhaust_storage": "w.exhaust_storage()", "restore_storage": "w.restore_storage()", "application_policy": "w.application_policy()", "request_query": "w.request_query()",
                "start_recovery": "w.start_recovery()", "save_call": "w.save_call()",
                "same_call": "w.same_call()", "saved_call_missing": "w.saved_call_missing()",
                "revoke": "w.revoke()",
                "conflict_command": "w.conflict_command()", "expire_command": "w.expire_command()",
                "read_result": "w.read_result()",
                "cancel_goal": "w.cancel_goal()",
                "complete_storage": "w.complete_storage()",
                "complete_old_writer": "w.complete_old_writer()",
                "execute": 'w.evidence.status("execute effect", w.service.execute(w.node.kernel()))'}[op]
    if op == "answer_query":
        keys(item, {"op", "result"})
        if item["result"] not in {"FOUND", "NOT_ACCEPTED", "UNKNOWN"}:
            raise FixtureError("unknown query result")
        return ('w.evidence.status("query answer", w.service.answer_query('
                f'AYNI_QUERY_RESULT_{item["result"]}))')
    if op == "create":
        keys(item, {"op", "type", "command"})
        if item["type"] not in {"folder", "action", "turn"}:
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
    keys(item, {"fixture_schema", "profile", "case_id", "experiment_step", "clauses", "script"},
         {"query_authority", "deployment"})
    if item.get("deployment", "counter") not in {"counter", "physical", "shared", "shared_physical", "replacement"}:
        raise FixtureError("unsupported deployment")
    if item.get("query_authority", "AUTHORITATIVE") not in {"NONE", "BEST_EFFORT", "AUTHORITATIVE"}:
        raise FixtureError("unsupported query authority")
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
    if item.get("deployment", "counter") not in {"shared","shared_physical"} and any(
            isinstance(a, dict) and (a.get("op") == "shared" or a.get("metric") == "shared_completed")
            for a in script):
        raise FixtureError("shared control/evidence requires shared deployment")
    if item.get("deployment", "counter") != "replacement" and any(
            isinstance(a, dict) and (a.get("op") == "replacement" or a.get("metric") == "replacement_completed")
            for a in script):
        raise FixtureError("replacement control/evidence requires replacement deployment")
    physical_metrics = {name for name in METRICS if name.startswith("physical_")} | {
        "held_renewals", "control_phase", "control_deadline"}
    if item.get("deployment", "counter") not in {"physical","shared_physical"} and any(
            isinstance(a, dict) and (a.get("op") == "physical" or a.get("metric") in physical_metrics)
            for a in script):
        raise FixtureError("physical control/evidence requires physical deployment")
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
                                            quote(",".join(item["clauses"])),
                                            'AYNI_CALL_QUERY_' + item.get("query_authority", "AUTHORITATIVE"),
                                            'Deployment::' + item.get("deployment", "counter"),
                                            f"scenario_{index}")) + "}")
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip()
    if subprocess.check_output(["git", "status", "--porcelain", "--untracked-files=normal"], cwd=root, text=True):
        revision += "+dirty"
    return ('#include "experiment_support.h"\n#include <ayni/runtime/sim_node/experiments.h>\n'
            '#include \"experiment_execution.h\"\n#include \"experiment_shared.h\"\n#include \"experiment_replacement.h\"\n'
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
