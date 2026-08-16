#!/usr/bin/env python3
"""Validate, plan, execute, summarize, or clean the frozen raw-host policy pilot.

Planning is the default and performs no GCP operation. Execution is bounded to
the twelve predeclared fresh VM-pair lifecycles and requires both billing and
reviewed-source acknowledgements.
"""

from __future__ import annotations

import argparse
import dataclasses
import hashlib
import json
from pathlib import Path
import re
import shutil
import sys
from typing import Iterable

import gcp_raw_host_emulation as harness


SCHEMA = "aurora-gcp-raw-host-policy-pilot-plan-v1"
MAX_LIFECYCLES = 12
ROOT = Path(__file__).resolve().parents[1]
DEFAULT_SPEC = ROOT / "benchmarks" / "gcp_raw_host_policy_pilot_v1.json"
ID_PATTERN = re.compile(r"^[a-z][a-z0-9-]{2,40}$")


class PilotError(RuntimeError):
    pass


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


@dataclasses.dataclass(frozen=True)
class Cell:
    id: str
    condition: str
    policy: str


@dataclasses.dataclass(frozen=True)
class Spec:
    path: Path
    value: dict
    cells: tuple[Cell, ...]

    @staticmethod
    def load(path: Path) -> "Spec":
        try:
            value = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as error:
            raise PilotError(f"invalid pilot specification: {path}") from error
        cells = tuple(
            Cell(item["id"], item["condition"], item["policy"])
            for item in value.get("cells", [])
        )
        result = Spec(path.resolve(), value, cells)
        result.validate()
        return result

    def validate(self) -> None:
        value = self.value
        if value.get("schema") != SCHEMA:
            raise PilotError("unexpected pilot schema")
        if value.get("pilot_id") != "raw-host-policy-pilot-v1":
            raise PilotError("unexpected pilot id")
        if value.get("purpose") != "bounded-policy-discrimination-pilot" or \
                value.get("confirmatory") is not False:
            raise PilotError("pilot must remain non-confirmatory")

        topology = value.get("topology", {})
        if topology.get("kind") != \
                "two-cross-region-non-peered-vpcs-public-ipv4":
            raise PilotError("pilot topology mismatch")
        if topology.get("sender_zone") == topology.get("receiver_zone"):
            raise PilotError("pilot zones must differ")

        workload = value.get("workload", {})
        if workload.get("id") != "policy-pilot-v1" or \
                workload.get("generation_count") != 8 or \
                workload.get("service") != \
                "sequential-plan-after-terminal-feedback" or \
                workload.get("symbol_size_bytes") != 64 or \
                workload.get("payload_bytes_per_generation") != 2560 or \
                workload.get("contract") != (
                    "deadline:30s;reliability:0.99;duty:0.1;rf:on;"
                    "optical:on;backscatter:on;ris:4;importance:elastic;"
                    "reserve:0.05;max_repair_amplification:4;"
                    "min_critical_overhead:1.5;"
                    "segment:0-1023,critical,10s,0.999;"
                    "segment:1024-2047,important,20s,0.99;seed:1701"
                ) or \
                harness.WORKLOAD_GENERATIONS.get(workload.get("id")) != 8:
            raise PilotError("pilot workload mismatch")

        policies = value.get("policies", [])
        policy_ids = {item.get("id") for item in policies}
        if policy_ids != set(harness.POLICY_IDS) or len(policies) != 3:
            raise PilotError("pilot must contain exactly the three policies")
        by_policy = {item["id"]: item for item in policies}
        if any(
            by_policy["fixed-minimum"].get(field) != expected
            for field, expected in (
                ("critical_overhead", 1.0),
                ("important_overhead", 1.0),
                ("elastic_overhead", 1.0),
            )
        ) or any(
            by_policy["fixed-class-aware"].get(field) != expected
            for field, expected in (
                ("critical_overhead", 2.5),
                ("important_overhead", 1.5),
                ("elastic_overhead", 1.1),
            )
        ) or any(
            by_policy["biological-adaptive"].get(field) != expected
            for field, expected in (
                ("alpha_up", 0.1),
                ("alpha_down", 0.02),
                ("panic_boost_generations", 3),
                ("gland_base_critical_overhead", 2.5),
                ("gland_base_important_overhead", 1.5),
            )
        ):
            raise PilotError("frozen policy definitions changed")

        conditions = value.get("conditions", [])
        condition_ids = {item.get("id") for item in conditions}
        expected_conditions = {"timed-replay-v2", "regime-change-v1"}
        if condition_ids != expected_conditions or len(conditions) != 2:
            raise PilotError("pilot must contain two adverse conditions")
        for condition in conditions:
            profile = harness.CONDITION_PROFILES.get(condition["id"])
            if profile is None or \
                    condition.get("forward_trace") != profile.forward_trace or \
                    condition.get("reverse_trace") != profile.reverse_trace or \
                    condition.get("outage_generation_index") != \
                    profile.outage_generation_index:
                raise PilotError("condition profile does not match the harness")
            for direction in ("forward", "reverse"):
                relative = condition[f"{direction}_trace"]
                expected_hash = condition[f"{direction}_trace_sha256"]
                if sha256_file(ROOT / relative) != expected_hash:
                    raise PilotError(f"frozen {direction} trace checksum mismatch")

        if value.get("blocks") != 2 or len(self.cells) != 6:
            raise PilotError("pilot requires six cells and two blocks")
        ids = [cell.id for cell in self.cells]
        if len(set(ids)) != len(ids) or not all(
            ID_PATTERN.fullmatch(cell_id) for cell_id in ids
        ):
            raise PilotError("pilot cell ids are invalid or duplicated")
        combinations = {(cell.condition, cell.policy) for cell in self.cells}
        expected = {
            (condition, policy)
            for condition in expected_conditions
            for policy in harness.POLICY_IDS
        }
        if combinations != expected:
            raise PilotError("pilot cells are not a complete policy-condition factorial")

        regime = next(
            item for item in conditions if item["id"] == "regime-change-v1"
        )
        if regime.get("outage_scope") != \
                "receiver-ingress-symbol-datagrams-only" or \
                regime.get("descriptors_and_feedback_preserved") is not True or \
                regime.get("outage_end") != \
                "receiver-local-critical-deadline" or \
                regime.get("policy_neutral") is not True:
            raise PilotError("regime-change condition is not frozen")

        randomization = value.get("randomization", {})
        seed = randomization.get("seed")
        if randomization.get("method") != \
                "randomized-complete-blocks-sha256-v1" or \
                not isinstance(seed, str) or not ID_PATTERN.fullmatch(seed):
            raise PilotError("pilot randomization is invalid")
        order = value.get("execution_order")
        if not isinstance(order, list) or len(order) != MAX_LIFECYCLES:
            raise PilotError("pilot execution order must contain 12 lifecycles")
        for block in (1, 2):
            observed = order[(block - 1) * 6:block * 6]
            expected_order = sorted(
                ids,
                key=lambda cell_id: hashlib.sha256(
                    f"{seed}:{block}:{cell_id}".encode("utf-8")
                ).hexdigest(),
            )
            if observed != expected_order:
                raise PilotError(f"pilot block {block} order is not frozen")

        measurement = value.get("measurement_schema", {})
        measurement_path = ROOT / measurement.get("path", "")
        if sha256_file(measurement_path) != measurement.get("sha256"):
            raise PilotError("frozen measurement schema checksum mismatch")
        measurement_value = json.loads(measurement_path.read_text(encoding="utf-8"))
        deadline_evaluation = measurement_value.get("deadline_evaluation", {})
        causal_regression = measurement_value.get(
            "causal_adaptation_regression", {}
        )
        if measurement_value.get("analysis", {}).get("superiority_test") is not False or \
                deadline_evaluation.get("clock") != "receiver-steady" or \
                deadline_evaluation.get("sender_expiry_values_used") is not False or \
                measurement_value.get("outcomes", {}).get(
                    "terminal_completion", {}
                ).get("protocol") != "authenticated-ack-v1" or \
                causal_regression.get("condition") != "regime-change-v1" or \
                causal_regression.get("policy_winner_targeted") is not False:
            raise PilotError("measurement schema is not pilot-bounded")

        causal_gate = value.get("causal_adaptation_gate", {})
        if causal_gate != {
            "condition": "regime-change-v1",
            "terminal_failure_generation_index": 2,
            "subsequent_plan_generation_index": 3,
            "biological_updated_state_required": True,
            "biological_protection_plan_change_required": True,
            "fixed_policy_plan_invariance_required": True,
        }:
            raise PilotError("causal adaptation gate is not frozen")

        execution = value.get("execution", {})
        safety = value.get("safety", {})
        if execution.get("lifecycle_count") != MAX_LIFECYCLES or \
                execution.get("fresh_vm_pair_per_cell_block") is not True or \
                execution.get("same_runtime_binary_sha256_across_treatments") is not True or \
                execution.get("process_protocol_version") != \
                harness.PROCESS_PROTOCOL_VERSION or \
                execution.get("terminal_handshake") != \
                "authenticated-ack-v1" or \
                execution.get("terminal_ack_occurs_after_policy_observation") \
                is not True or \
                safety.get("maximum_lifecycles") != MAX_LIFECYCLES or \
                safety.get("replacement_lifecycles") is not False:
            raise PilotError("pilot lifecycle safety bounds are incomplete")


@dataclasses.dataclass(frozen=True)
class Context:
    project: str
    run_prefix: str
    source_commit: str
    repository_url: str
    evidence_root: Path


@dataclasses.dataclass(frozen=True)
class PlannedRun:
    sequence: int
    block: int
    cell: Cell
    run_id: str
    evidence_dir: Path


def planned_runs(spec: Spec, context: Context) -> list[PlannedRun]:
    cells = {cell.id: cell for cell in spec.cells}
    runs = []
    for sequence, cell_id in enumerate(spec.value["execution_order"], start=1):
        block = (sequence - 1) // len(spec.cells) + 1
        run_id = f"{context.run_prefix}-b{block}r{sequence:02d}"
        runs.append(
            PlannedRun(
                sequence,
                block,
                cells[cell_id],
                run_id,
                context.evidence_root / f"{sequence:02d}-{cell_id}",
            )
        )
    return runs


def run_config(spec: Spec, context: Context, run: PlannedRun) -> harness.Config:
    topology = spec.value["topology"]
    return harness.Config(
        project=context.project,
        run_id=run.run_id,
        sender_zone=topology["sender_zone"],
        receiver_zone=topology["receiver_zone"],
        machine_type=topology["machine_type"],
        condition_profile=run.cell.condition,
        source_commit=context.source_commit,
        repository_url=context.repository_url,
        evidence_dir=run.evidence_dir,
        policy_id=run.cell.policy,
        workload_id=spec.value["workload"]["id"],
    )


def validate_context(spec: Spec, context: Context) -> None:
    if not ID_PATTERN.fullmatch(context.run_prefix):
        raise PilotError("run prefix must be 3..31 safe lowercase characters")
    for run in planned_runs(spec, context):
        try:
            run_config(spec, context, run).validate()
            harness.Names.from_run_id(run.run_id)
        except harness.HarnessError as error:
            raise PilotError(f"invalid run {run.run_id}: {error}") from error


def build_plan(spec: Spec, context: Context) -> dict:
    validate_context(spec, context)
    runs = planned_runs(spec, context)
    return {
        "schema": "aurora-gcp-raw-host-policy-pilot-execution-plan-v1",
        "pilot_id": spec.value["pilot_id"],
        "project": context.project,
        "source_commit": context.source_commit,
        "topology": spec.value["topology"],
        "workload": spec.value["workload"],
        "measurement_schema": spec.value["measurement_schema"],
        "lifecycles": [
            {
                "sequence": run.sequence,
                "block": run.block,
                "cell": run.cell.id,
                "condition": run.cell.condition,
                "policy": run.cell.policy,
                "run_id": run.run_id,
            }
            for run in runs
        ],
        "lifecycle_count": len(runs),
        "safety": {
            "default_mode": "plan-only",
            "fresh_vm_pair_per_lifecycle": True,
            "maximum_lifecycles": MAX_LIFECYCLES,
            "reviewed_source_commit_required_for_execute": True,
            "replacement_lifecycles": False,
        },
        "claims": spec.value["claims"],
    }


def summarize(spec: Spec, context: Context) -> dict:
    common_binary = None
    cells: dict[str, dict] = {}
    for run in planned_runs(spec, context):
        path = run.evidence_dir / "manifest.json"
        try:
            value = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as error:
            raise PilotError(f"invalid evidence for {run.run_id}") from error
        if value.get("result") != "passed" or \
                value.get("teardown", {}).get("success") is not True:
            raise PilotError(f"{run.run_id}: failed lifecycle or teardown")
        if value.get("schema") != harness.RAW_EVIDENCE_SCHEMA or \
                value.get("process_protocol_version") != \
                harness.PROCESS_PROTOCOL_VERSION or \
                value.get("terminal_handshake") != \
                "authenticated-ack-v1" or \
                value.get("measurement", {}).get(
                    "process_protocol_version"
                ) != harness.PROCESS_PROTOCOL_VERSION:
            raise PilotError(f"{run.run_id}: process evidence schema mismatch")
        if value.get("source_commit") != context.source_commit or \
                value.get("run_id") != run.run_id:
            raise PilotError(f"{run.run_id}: evidence identity mismatch")
        if value.get("treatment", {}).get("policy_id") != run.cell.policy or \
                value.get("condition", {}).get("profile") != run.cell.condition or \
                value.get("workload", {}).get("id") != "policy-pilot-v1":
            raise PilotError(f"{run.run_id}: frozen factor mismatch")
        runtime_hash = value.get("runtime_binary_sha256")
        if common_binary is None:
            common_binary = runtime_hash
        elif runtime_hash != common_binary:
            raise PilotError("treatments did not use one identical binary")
        timing = value.get("timing", {})
        required = (
            "delivered",
            "critical_generations",
            "critical_delivered_before_deadline",
            "source_symbols",
            "initial_symbols",
            "repair_symbols_requested",
            "repair_symbols_emitted",
            "wire_symbol_datagrams",
            "terminal_feedback_rtt_mean_us",
            "terminal_ack_datagrams",
            "terminal_acknowledged",
            "terminal_feedback_retry_rounds",
            "terminal_ack_wait_ms",
        )
        if any(not isinstance(timing.get(field), (int, float)) for field in required):
            raise PilotError(f"{run.run_id}: incomplete pilot measurements")
        cell = cells.setdefault(
            run.cell.id,
            {"policy": run.cell.policy, "condition": run.cell.condition, "runs": []},
        )
        cell["runs"].append({"run_id": run.run_id, "block": run.block, **{
            field: timing[field] for field in required
        }})
    return {
        "schema": "aurora-raw-host-policy-pilot-summary-v1",
        "pilot_id": spec.value["pilot_id"],
        "source_commit": context.source_commit,
        "runtime_binary_sha256": common_binary,
        "lifecycles_observed": MAX_LIFECYCLES,
        "cells": [cells[cell.id] for cell in spec.cells],
        "analysis": spec.value["analysis"],
        "claims": spec.value["claims"],
    }


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--spec", type=Path, default=DEFAULT_SPEC)
    result.add_argument("--project", required=True)
    result.add_argument("--run-prefix", default="policy-pilot")
    result.add_argument("--source-commit", default=harness.current_commit())
    result.add_argument(
        "--repository-url",
        default="https://github.com/Daniele-Cangi/Aurora.git",
    )
    result.add_argument("--evidence-root", type=Path, default=Path("raw-host-evidence/policy-pilot-v1"))
    result.add_argument("--execute", action="store_true")
    result.add_argument("--summarize", action="store_true")
    result.add_argument("--cleanup-only", action="store_true")
    result.add_argument("--acknowledge-billing-and-teardown", action="store_true")
    result.add_argument("--reviewed-source-commit")
    result.add_argument("--quiet", action="store_true")
    return result


def main(argv: Iterable[str] | None = None) -> int:
    args = parser().parse_args(argv)
    spec = Spec.load(args.spec)
    context = Context(
        args.project,
        args.run_prefix,
        args.source_commit,
        args.repository_url,
        args.evidence_root,
    )
    plan = build_plan(spec, context)
    if args.summarize:
        print(json.dumps(summarize(spec, context), indent=2, sort_keys=True))
        return 0
    if not args.execute and not args.cleanup_only:
        print(json.dumps(plan, indent=2, sort_keys=True))
        return 0
    if not args.acknowledge_billing_and_teardown:
        raise PilotError("execution and cleanup require billing/teardown acknowledgement")
    gcloud = shutil.which("gcloud")
    if not gcloud:
        raise PilotError("gcloud was not found")
    runner = harness.CommandRunner(verbose=not args.quiet)
    if args.cleanup_only:
        passed = True
        for run in planned_runs(spec, context):
            result = harness.GcpRawHarness(
                run_config(spec, context, run), runner, gcloud=gcloud
            ).cleanup()
            passed = passed and result["success"]
        return 0 if passed else 1
    if args.reviewed_source_commit != context.source_commit:
        raise PilotError(
            "execution requires --reviewed-source-commit equal to --source-commit"
        )
    for run in planned_runs(spec, context):
        harness.GcpRawHarness(
            run_config(spec, context, run), runner, gcloud=gcloud
        ).execute()
    summary = summarize(spec, context)
    context.evidence_root.mkdir(parents=True, exist_ok=True)
    (context.evidence_root / "summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(json.dumps(summary, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (PilotError, harness.HarnessError) as error:
        print(f"gcp raw policy pilot: {error}", file=sys.stderr)
        raise SystemExit(1)
