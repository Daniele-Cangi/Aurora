#!/usr/bin/env python3
"""Plan, validate, execute, summarize, or clean the post-shock study.

Planning is the default and performs no GCP operation. Execution is bounded to
the 46 predeclared fresh VM-pair lifecycles and requires billing, reviewed-
source and immutable-tag acknowledgements.
"""

from __future__ import annotations

import argparse
import dataclasses
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
from typing import Iterable

import gcp_raw_host_emulation as harness
from gcp_raw_post_shock_evidence import EvidenceError, validate_lifecycle


SCHEMA = "aurora-gcp-raw-post-shock-efficiency-study-plan-v1"
STUDY_ID = "raw-host-post-shock-efficiency-study-v1"
BLOCKS = 23
CELLS_PER_BLOCK = 2
MAX_LIFECYCLES = BLOCKS * CELLS_PER_BLOCK
ROOT = Path(__file__).resolve().parents[1]
DEFAULT_SPEC = (
    ROOT / "benchmarks" / "gcp_raw_post_shock_efficiency_study_v1.json"
)
ID_PATTERN = re.compile(r"^[a-z][a-z0-9-]{2,40}$")
TAG_PATTERN = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,100}$")
PRIMARY_POLICIES = ("fixed-class-aware", "biological-adaptive")


class StudyError(RuntimeError):
    pass


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(block)
    except OSError as error:
        raise StudyError(f"cannot hash frozen file: {path}") from error
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
            cells = tuple(
                Cell(item["id"], item["condition"], item["policy"])
                for item in value.get("cells", [])
            )
        except (OSError, json.JSONDecodeError, KeyError, TypeError) as error:
            raise StudyError(f"invalid study specification: {path}") from error
        result = Spec(path.resolve(), value, cells)
        result.validate()
        return result

    def validate(self) -> None:
        value = self.value
        if value.get("schema") != SCHEMA or value.get("study_id") != STUDY_ID:
            raise StudyError("unexpected post-shock study identity")
        if value.get("purpose") != "confirmatory-post-shock-efficiency-study" or \
                value.get("confirmatory") is not True or \
                value.get("status") != "proposed-freeze-pending-review":
            raise StudyError("study review status or purpose changed")

        provenance = value.get("pilot_provenance", {})
        if provenance.get("release") != "raw-host-policy-pilot-v1-study-v4" or \
                provenance.get("experimental_source") != \
                "c523e8020c7d96587ce37638d82d728a2513f04c" or \
                provenance.get("evidence_archive_sha256") != \
                "bacad78c1c678c64b8c9c605186969d54db33904ef8a347d249b12b1a4112c99":
            raise StudyError("pilot provenance changed")
        planning_path = ROOT / provenance.get("planning_input_path", "")
        if sha256_file(planning_path) != provenance.get("planning_input_sha256"):
            raise StudyError("pilot planning input checksum mismatch")

        topology = value.get("topology", {})
        if topology.get("kind") != \
                "two-cross-region-non-peered-vpcs-public-ipv4" or \
                topology.get("sender_zone") != "us-east1-b" or \
                topology.get("receiver_zone") != "us-west1-b" or \
                topology.get("machine_type") != "e2-micro":
            raise StudyError("study topology changed")
        workload = value.get("workload", {})
        expected_contract = (
            "deadline:30s;reliability:0.99;duty:0.1;rf:on;optical:on;"
            "backscatter:on;ris:4;importance:elastic;reserve:0.05;"
            "max_repair_amplification:4;min_critical_overhead:1.5;"
            "segment:0-1023,critical,10s,0.999;"
            "segment:1024-2047,important,20s,0.99;seed:1701"
        )
        if workload.get("id") != "policy-pilot-v1" or \
                workload.get("generation_count") != 8 or \
                workload.get("service") != \
                "sequential-plan-after-terminal-feedback" or \
                workload.get("symbol_size_bytes") != 64 or \
                workload.get("payload_bytes_per_generation") != 2560 or \
                workload.get("contract") != expected_contract or \
                harness.WORKLOAD_GENERATIONS.get(workload.get("id")) != 8:
            raise StudyError("study workload changed")

        definitions = value.get("policy_definitions", [])
        if not isinstance(definitions, list) or \
                {item.get("id") for item in definitions} != \
                set(harness.POLICY_IDS) or len(definitions) != 3:
            raise StudyError("all three frozen policy definitions are required")
        policies = {item["id"]: item for item in definitions}
        expected_fields = {
            "fixed-minimum": {
                "critical_overhead": 1.0,
                "important_overhead": 1.0,
                "elastic_overhead": 1.0,
            },
            "fixed-class-aware": {
                "critical_overhead": 2.5,
                "important_overhead": 1.5,
                "elastic_overhead": 1.1,
            },
            "biological-adaptive": {
                "alpha_up": 0.1,
                "alpha_down": 0.02,
                "panic_boost_generations": 3,
                "gland_base_critical_overhead": 2.5,
                "gland_base_important_overhead": 1.5,
            },
        }
        for policy, fields in expected_fields.items():
            definition = policies[policy]
            if definition.get("version") != 1 or \
                    definition.get("contract_floors_and_caps_apply") is not True or \
                    any(definition.get(field) != expected
                        for field, expected in fields.items()):
                raise StudyError(f"frozen {policy} definition changed")
        if value.get("treatments") != list(PRIMARY_POLICIES) or \
                policies["fixed-minimum"].get("study_role") != "not-dispatched":
            raise StudyError("powered treatment set changed")

        condition = value.get("condition", {})
        profile = harness.CONDITION_PROFILES.get(condition.get("id"))
        if profile is None or condition.get("id") != "regime-change-v1" or \
                condition.get("forward_trace") != profile.forward_trace or \
                condition.get("reverse_trace") != profile.reverse_trace or \
                condition.get("outage_generation_index") != 2 or \
                profile.outage_generation_index != 2 or \
                condition.get("outage_scope") != \
                "receiver-ingress-symbol-datagrams-only" or \
                condition.get("descriptors_and_feedback_preserved") is not True or \
                condition.get("outage_end") != \
                "receiver-local-critical-deadline" or \
                condition.get("policy_neutral") is not True:
            raise StudyError("regime-change condition changed")
        for direction in ("forward", "reverse"):
            if sha256_file(ROOT / condition[f"{direction}_trace"]) != \
                    condition.get(f"{direction}_trace_sha256"):
                raise StudyError(f"{direction} trace checksum mismatch")

        if value.get("blocks") != BLOCKS or len(self.cells) != CELLS_PER_BLOCK:
            raise StudyError("study must contain 23 paired complete blocks")
        ids = [cell.id for cell in self.cells]
        if len(set(ids)) != len(ids) or not all(
            ID_PATTERN.fullmatch(cell_id) for cell_id in ids
        ):
            raise StudyError("study cell ids are invalid or duplicated")
        if {(cell.condition, cell.policy) for cell in self.cells} != {
            ("regime-change-v1", policy) for policy in PRIMARY_POLICIES
        }:
            raise StudyError("study cells do not form the registered policy pair")

        randomization = value.get("randomization", {})
        seed = randomization.get("seed")
        if randomization.get("method") != \
                "randomized-complete-blocks-sha256-v1" or \
                not isinstance(seed, str) or not ID_PATTERN.fullmatch(seed):
            raise StudyError("study randomization changed")
        order = value.get("execution_order")
        if not isinstance(order, list) or len(order) != MAX_LIFECYCLES:
            raise StudyError("study execution order must contain 46 lifecycles")
        for block in range(1, BLOCKS + 1):
            observed = order[
                (block - 1) * CELLS_PER_BLOCK:block * CELLS_PER_BLOCK
            ]
            expected = sorted(
                ids,
                key=lambda cell_id: hashlib.sha256(
                    f"{seed}:{block}:{cell_id}".encode("utf-8")
                ).hexdigest(),
            )
            if observed != expected:
                raise StudyError(f"study block {block} order changed")

        measurement = value.get("measurement_schema", {})
        measurement_path = ROOT / measurement.get("path", "")
        if sha256_file(measurement_path) != measurement.get("sha256"):
            raise StudyError("measurement schema checksum mismatch")
        try:
            measurement_value = json.loads(
                measurement_path.read_text(encoding="utf-8")
            )
        except (OSError, json.JSONDecodeError) as error:
            raise StudyError("invalid measurement schema") from error
        primary = measurement_value.get("primary_outcome", {})
        deadline = measurement_value.get("deadline_evaluation", {})
        measurement_analysis = measurement_value.get("analysis", {})
        if measurement_value.get("schema") != \
                "aurora-raw-host-post-shock-efficiency-measurement-v1" or \
                primary.get("denominator") != \
                "five-prescheduled-post-shock-generations" or \
                deadline.get("clock") != "receiver-steady" or \
                deadline.get("sender_expiry_values_used") is not False or \
                measurement_analysis.get("superiority_test") is not True or \
                measurement_analysis.get("outcome_dependent_stopping") is not False:
            raise StudyError("registered measurement semantics changed")

        visualization = value.get("visualization_spec", {})
        visualization_path = ROOT / visualization.get("path", "")
        if sha256_file(visualization_path) != visualization.get("sha256"):
            raise StudyError("visualization specification checksum mismatch")
        try:
            visualization_value = json.loads(
                visualization_path.read_text(encoding="utf-8")
            )
        except (OSError, json.JSONDecodeError) as error:
            raise StudyError("invalid visualization specification") from error
        figure_ids = {
            figure.get("id")
            for figure in visualization_value.get("figures", [])
            if isinstance(figure, dict)
        }
        constraints = visualization_value.get(
            "presentation_constraints", {}
        )
        if visualization_value.get("schema") != \
                "aurora-raw-host-post-shock-efficiency-visualization-v1" or \
                visualization_value.get("study_id") != STUDY_ID or \
                visualization_value.get("registered_before_dispatch") is not True or \
                figure_ids != {
                    "primary-paired-contrast",
                    "generation-protection-response",
                    "post-shock-wire-composition",
                    "delivery-guardrail",
                } or constraints.get("show_all_blocks") is not True or \
                constraints.get("rank_generation_2_by_policy") is not False or \
                constraints.get("hide_delivery_misses") is not False or \
                constraints.get("outcome_dependent_palette_or_order") is not False:
            raise StudyError("registered visual analysis semantics changed")

        power = value.get("power", {})
        if power.get("test") != \
                "two-sided-one-sample-t-on-paired-block-contrasts" or \
                power.get("alpha") != 0.05 or \
                power.get("target_power") != 0.90 or \
                power.get("minimum_relevant_effect_wire_symbols") != 50 or \
                power.get("planning_standard_deviation_wire_symbols") != 70 or \
                power.get("required_complete_blocks") != BLOCKS:
            raise StudyError("registered power design changed")
        analysis = value.get("analysis", {})
        if analysis.get("post_shock_generation_indices") != [3, 4, 5, 6, 7] or \
                analysis.get("shock_generation_index") != 2 or \
                analysis.get("shock_excluded_from_policy_comparison") is not True or \
                analysis.get("outcome_dependent_stopping") is not False or \
                analysis.get("outcome_dependent_replacement") is not False or \
                analysis.get("delivery_superiority_study_deferred") is not True:
            raise StudyError("registered analysis changed")

        causal = value.get("causal_adaptation_gate", {})
        if causal.get("condition") != "regime-change-v1" or \
                causal.get("pre_shock_success_generation_indices") != [0, 1] or \
                causal.get("terminal_failure_generation_index") != 2 or \
                causal.get("subsequent_plan_generation_index") != 3 or \
                causal.get("biological_updated_state_required") is not True or \
                causal.get("biological_protection_plan_change_required") is not True or \
                causal.get("fixed_policy_plan_invariance_required") is not True:
            raise StudyError("causal adaptation gate changed")

        execution = value.get("execution", {})
        safety = value.get("safety", {})
        freeze = value.get("evidence_freeze", {})
        if execution.get("lifecycle_count") != MAX_LIFECYCLES or \
                execution.get("fresh_vm_pair_per_cell_block") is not True or \
                execution.get("same_runtime_binary_sha256_across_treatments") \
                is not True or \
                execution.get("process_protocol_version") != 3 or \
                execution.get("terminal_handshake") != "authenticated-ack-v1" or \
                execution.get("immutable_experimental_tag_required_before_execute") \
                is not True or \
                safety.get("maximum_lifecycles") != MAX_LIFECYCLES or \
                safety.get("stop_on_first_invalid_lifecycle_or_cleanup") is not True or \
                safety.get("replacement_lifecycles") is not False or \
                freeze.get("archive_and_sha256_before_outcome_analysis") is not True:
            raise StudyError("study execution or freeze safety changed")


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
        block = (sequence - 1) // CELLS_PER_BLOCK + 1
        run_id = f"{context.run_prefix}-b{block:02d}r{sequence:02d}"
        runs.append(PlannedRun(
            sequence,
            block,
            cells[cell_id],
            run_id,
            context.evidence_root / f"{sequence:02d}-{cell_id}",
        ))
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
        raise StudyError("run prefix must be 3..41 safe lowercase characters")
    if not re.fullmatch(r"[0-9a-f]{40}", context.source_commit):
        raise StudyError("source commit must be a full lowercase Git SHA")
    for run in planned_runs(spec, context):
        try:
            run_config(spec, context, run).validate()
            harness.Names.from_run_id(run.run_id)
        except harness.HarnessError as error:
            raise StudyError(f"invalid run {run.run_id}: {error}") from error


def build_plan(spec: Spec, context: Context) -> dict:
    validate_context(spec, context)
    runs = planned_runs(spec, context)
    return {
        "schema": "aurora-gcp-raw-post-shock-execution-plan-v1",
        "study_id": spec.value["study_id"],
        "study_spec_sha256": sha256_file(spec.path),
        "project": context.project,
        "source_commit": context.source_commit,
        "topology": spec.value["topology"],
        "workload": spec.value["workload"],
        "condition": spec.value["condition"],
        "measurement_schema": spec.value["measurement_schema"],
        "visualization_spec": spec.value["visualization_spec"],
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
            "immutable_experimental_tag_required_for_execute": True,
            "stop_on_first_invalid_lifecycle_or_cleanup": True,
            "replacement_lifecycles": False,
        },
        "analysis": spec.value["analysis"],
        "claims": spec.value["claims"],
    }


def summarize_collection(spec: Spec, context: Context) -> dict:
    common_binary = None
    runs = []
    for run in planned_runs(spec, context):
        result = validate_lifecycle(
            run.evidence_dir,
            run_id=run.run_id,
            source_commit=context.source_commit,
            policy=run.cell.policy,
        )
        runtime_hash = result["runtime_binary_sha256"]
        if common_binary is None:
            common_binary = runtime_hash
        elif runtime_hash != common_binary:
            raise StudyError("treatments did not use one identical binary")
        runs.append({
            "sequence": run.sequence,
            "block": run.block,
            "run_id": run.run_id,
            "policy": run.cell.policy,
            "technical_validation": "passed",
            "runtime_binary_sha256": runtime_hash,
        })
    return {
        "schema": "aurora-raw-post-shock-collection-summary-v1",
        "study_id": spec.value["study_id"],
        "source_commit": context.source_commit,
        "runtime_binary_sha256": common_binary,
        "lifecycles_observed": len(runs),
        "outcome_analysis_performed": False,
        "evidence_freeze_required_before_outcome_analysis": True,
        "runs": runs,
    }


def resolve_tag_commit(tag: str) -> str:
    if not TAG_PATTERN.fullmatch(tag):
        raise StudyError("experimental tag contains unsafe characters")
    result = subprocess.run(
        [
            "git",
            "-c",
            f"safe.directory={ROOT.as_posix()}",
            "-C",
            str(ROOT),
            "rev-parse",
            "--verify",
            f"refs/tags/{tag}^{{commit}}",
        ],
        check=False,
        capture_output=True,
        text=True,
    )
    commit = result.stdout.strip()
    if result.returncode != 0 or not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise StudyError(f"immutable experimental tag was not found: {tag}")
    return commit


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--spec", type=Path, default=DEFAULT_SPEC)
    result.add_argument("--project", required=True)
    result.add_argument("--run-prefix", default="postshock-v1")
    result.add_argument("--source-commit", default=harness.current_commit())
    result.add_argument(
        "--repository-url",
        default="https://github.com/Daniele-Cangi/Aurora.git",
    )
    result.add_argument(
        "--evidence-root",
        type=Path,
        default=Path("raw-host-evidence/post-shock-efficiency-v1"),
    )
    result.add_argument("--execute", action="store_true")
    result.add_argument("--summarize", action="store_true")
    result.add_argument("--cleanup-only", action="store_true")
    result.add_argument("--acknowledge-billing-and-teardown", action="store_true")
    result.add_argument("--reviewed-source-commit")
    result.add_argument("--experimental-tag")
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
        print(json.dumps(
            summarize_collection(spec, context), indent=2, sort_keys=True
        ))
        return 0
    if not args.execute and not args.cleanup_only:
        print(json.dumps(plan, indent=2, sort_keys=True))
        return 0
    if not args.acknowledge_billing_and_teardown:
        raise StudyError("execution and cleanup require billing/teardown acknowledgement")
    gcloud = shutil.which("gcloud")
    if not gcloud:
        raise StudyError("gcloud was not found")
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
        raise StudyError(
            "execution requires --reviewed-source-commit equal to --source-commit"
        )
    if args.experimental_tag is None or \
            resolve_tag_commit(args.experimental_tag) != context.source_commit:
        raise StudyError("experimental tag must resolve to the reviewed source commit")
    for run in planned_runs(spec, context):
        harness.GcpRawHarness(
            run_config(spec, context, run), runner, gcloud=gcloud
        ).execute()
        validate_lifecycle(
            run.evidence_dir,
            run_id=run.run_id,
            source_commit=context.source_commit,
            policy=run.cell.policy,
        )
    summary = summarize_collection(spec, context)
    context.evidence_root.mkdir(parents=True, exist_ok=True)
    (context.evidence_root / "collection-summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(json.dumps(summary, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (StudyError, EvidenceError, harness.HarnessError) as error:
        print(f"gcp raw post-shock study: {error}", file=sys.stderr)
        raise SystemExit(1)
