#!/usr/bin/env python3
"""Plan, run, validate, and clean a controlled Aurora GCP factor matrix.

The default mode is plan-only. Execution and cleanup require the same explicit
billing/teardown acknowledgement as the single-run raw-host harness. Matrix
cells form a complete balanced region-pair by condition-profile factorial.
"""

from __future__ import annotations

import argparse
import dataclasses
import hashlib
import json
from pathlib import Path
import re
import shutil
from typing import Iterable

import gcp_raw_host_emulation as harness
from summarize_gcp_raw_campaign import TIMING_FIELDS, metric_summary


MATRIX_SCHEMA = "aurora-gcp-raw-matrix-plan-v1"
MATRIX_SCHEMA_V2 = "aurora-gcp-raw-matrix-plan-v2"
EVIDENCE_SCHEMA = "aurora-raw-host-matrix-evidence-v2"
MAX_MATRIX_SAMPLES = 12
ID_PATTERN = re.compile(r"^[a-z][a-z0-9-]{2,40}$")


class MatrixError(RuntimeError):
    pass


@dataclasses.dataclass(frozen=True)
class MatrixCell:
    id: str
    sender_zone: str
    receiver_zone: str
    condition_profile: str
    samples: int

    @property
    def region_pair(self) -> tuple[str, str]:
        return (
            harness.zone_region(self.sender_zone),
            harness.zone_region(self.receiver_zone),
        )


@dataclasses.dataclass(frozen=True)
class MatrixSpec:
    schema: str
    matrix_id: str
    machine_type: str
    cells: tuple[MatrixCell, ...]
    execution_order: tuple[str, ...] = ()
    randomization_method: str | None = None
    randomization_seed: str | None = None

    @staticmethod
    def load(path: Path) -> "MatrixSpec":
        try:
            value = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as error:
            raise MatrixError(f"invalid matrix file: {path}") from error
        schema = value.get("schema")
        if schema not in (MATRIX_SCHEMA, MATRIX_SCHEMA_V2):
            raise MatrixError("unexpected matrix schema")
        cells_value = value.get("cells")
        if not isinstance(cells_value, list):
            raise MatrixError("matrix cells must be a list")
        cells: list[MatrixCell] = []
        for index, item in enumerate(cells_value, start=1):
            if not isinstance(item, dict):
                raise MatrixError(f"cell {index} must be an object")
            try:
                cell = MatrixCell(
                    id=item["id"],
                    sender_zone=item["sender_zone"],
                    receiver_zone=item["receiver_zone"],
                    condition_profile=item["condition_profile"],
                    samples=item["samples"],
                )
            except KeyError as error:
                raise MatrixError(f"cell {index} is incomplete") from error
            cells.append(cell)
        try:
            execution_order = value.get("execution_order", [])
            randomization = value.get("randomization", {})
            if not isinstance(execution_order, list) or not all(
                isinstance(item, str) for item in execution_order
            ):
                raise MatrixError("execution order must be a string list")
            if not isinstance(randomization, dict):
                raise MatrixError("randomization must be an object")
            result = MatrixSpec(
                schema=schema,
                matrix_id=value["matrix_id"],
                machine_type=value["machine_type"],
                cells=tuple(cells),
                execution_order=tuple(execution_order),
                randomization_method=randomization.get("method"),
                randomization_seed=randomization.get("seed"),
            )
        except KeyError as error:
            raise MatrixError("matrix metadata is incomplete") from error
        result.validate()
        return result

    def validate(self) -> None:
        if not isinstance(self.matrix_id, str) or not ID_PATTERN.fullmatch(
            self.matrix_id
        ):
            raise MatrixError("invalid matrix id")
        if not isinstance(self.machine_type, str) or not re.fullmatch(
            r"[a-z][a-z0-9-]{1,30}", self.machine_type
        ):
            raise MatrixError("invalid matrix machine type")
        if not self.cells:
            raise MatrixError("matrix must contain cells")

        ids: set[str] = set()
        combinations: set[tuple[str, str, str]] = set()
        region_pairs: set[tuple[str, str]] = set()
        conditions: set[str] = set()
        sample_counts: set[int] = set()
        total_samples = 0
        zone_pattern = re.compile(r"[a-z]+-[a-z]+[0-9]-[a-z]")
        for cell in self.cells:
            if not isinstance(cell.id, str) or not ID_PATTERN.fullmatch(cell.id):
                raise MatrixError("invalid cell id")
            if cell.id in ids:
                raise MatrixError("matrix cell ids must be unique")
            ids.add(cell.id)
            if not isinstance(cell.sender_zone, str) or not zone_pattern.fullmatch(
                cell.sender_zone
            ):
                raise MatrixError(f"{cell.id}: invalid sender zone")
            if not isinstance(cell.receiver_zone, str) or not zone_pattern.fullmatch(
                cell.receiver_zone
            ):
                raise MatrixError(f"{cell.id}: invalid receiver zone")
            if cell.sender_zone == cell.receiver_zone:
                raise MatrixError(f"{cell.id}: zones must differ")
            if cell.condition_profile not in harness.CONDITION_PROFILES:
                raise MatrixError(f"{cell.id}: unknown condition profile")
            if type(cell.samples) is not int or not 1 <= cell.samples <= 10:
                raise MatrixError(f"{cell.id}: samples must be in 1..10")
            combination = (
                cell.sender_zone,
                cell.receiver_zone,
                cell.condition_profile,
            )
            if combination in combinations:
                raise MatrixError("matrix factor combinations must be unique")
            combinations.add(combination)
            region_pairs.add((cell.sender_zone, cell.receiver_zone))
            conditions.add(cell.condition_profile)
            sample_counts.add(cell.samples)
            total_samples += cell.samples

        if len(region_pairs) < 2 or len(conditions) < 2:
            raise MatrixError(
                "matrix requires at least two region pairs and two conditions"
            )
        expected = {
            (sender, receiver, condition)
            for sender, receiver in region_pairs
            for condition in conditions
        }
        if combinations != expected:
            raise MatrixError("matrix must be a complete region/condition factorial")
        if len(sample_counts) != 1:
            raise MatrixError("matrix must use a balanced sample count")
        if total_samples > MAX_MATRIX_SAMPLES:
            raise MatrixError(
                f"matrix exceeds the {MAX_MATRIX_SAMPLES}-sample safety bound"
            )
        if self.schema == MATRIX_SCHEMA:
            if self.execution_order or self.randomization_method or \
                    self.randomization_seed:
                raise MatrixError("v1 matrices cannot declare execution order")
            return

        if self.randomization_method != "randomized-complete-blocks-sha256-v1":
            raise MatrixError("v2 matrix requires SHA-256 complete-block order")
        if not isinstance(self.randomization_seed, str) or not ID_PATTERN.fullmatch(
            self.randomization_seed
        ):
            raise MatrixError("v2 matrix requires a safe randomization seed")
        if len(self.execution_order) != total_samples:
            raise MatrixError("v2 execution order length mismatch")
        samples_per_cell = next(iter(sample_counts))
        cell_ids = [cell.id for cell in self.cells]
        for block_index in range(1, samples_per_cell + 1):
            start = (block_index - 1) * len(cell_ids)
            observed = list(
                self.execution_order[start:start + len(cell_ids)]
            )
            expected = sorted(
                cell_ids,
                key=lambda cell_id: hashlib.sha256(
                    f"{self.randomization_seed}:{block_index}:{cell_id}".encode(
                        "utf-8"
                    )
                ).hexdigest(),
            )
            if observed != expected:
                raise MatrixError(
                    f"v2 execution block {block_index} is not the declared order"
                )

    @property
    def total_samples(self) -> int:
        return sum(cell.samples for cell in self.cells)


@dataclasses.dataclass(frozen=True)
class RunContext:
    project: str
    run_prefix: str
    source_commit: str
    repository_url: str
    evidence_root: Path


@dataclasses.dataclass(frozen=True)
class PlannedRun:
    cell_index: int
    sample_index: int
    cell: MatrixCell
    run_id: str
    evidence_dir: Path


def planned_runs(spec: MatrixSpec, context: RunContext) -> list[PlannedRun]:
    result: list[PlannedRun] = []
    indexed_cells = {
        cell.id: (index, cell)
        for index, cell in enumerate(spec.cells, start=1)
    }
    sample_counts = {cell.id: 0 for cell in spec.cells}
    cell_order = spec.execution_order or tuple(
        cell.id
        for cell in spec.cells
        for _ in range(cell.samples)
    )
    for cell_id in cell_order:
        cell_index, cell = indexed_cells[cell_id]
        sample_counts[cell_id] += 1
        sample_index = sample_counts[cell_id]
        run_id = (
            f"{context.run_prefix}-c{cell_index:02d}s{sample_index:02d}"
        )
        evidence_dir = (
            context.evidence_root
            / f"cell-{cell_index:02d}-{cell.id}"
            / f"sample-{sample_index:02d}"
        )
        result.append(
            PlannedRun(
                cell_index=cell_index,
                sample_index=sample_index,
                cell=cell,
                run_id=run_id,
                evidence_dir=evidence_dir,
            )
        )
    return result


def run_config(spec: MatrixSpec, context: RunContext, run: PlannedRun):
    return harness.Config(
        project=context.project,
        run_id=run.run_id,
        sender_zone=run.cell.sender_zone,
        receiver_zone=run.cell.receiver_zone,
        machine_type=spec.machine_type,
        condition_profile=run.cell.condition_profile,
        source_commit=context.source_commit,
        repository_url=context.repository_url,
        evidence_dir=run.evidence_dir,
    )


def validate_context(spec: MatrixSpec, context: RunContext) -> None:
    runs = planned_runs(spec, context)
    if not runs:
        raise MatrixError("matrix produced no runs")
    for run in runs:
        try:
            run_config(spec, context, run).validate()
            harness.Names.from_run_id(run.run_id)
        except harness.HarnessError as error:
            raise MatrixError(f"invalid planned run {run.run_id}: {error}") from error


def build_plan(spec: MatrixSpec, context: RunContext) -> dict:
    validate_context(spec, context)
    runs = planned_runs(spec, context)
    return {
        "schema": (
            "aurora-gcp-raw-matrix-execution-plan-v2"
            if spec.schema == MATRIX_SCHEMA_V2
            else "aurora-gcp-raw-matrix-execution-plan-v1"
        ),
        "matrix_id": spec.matrix_id,
        "project": context.project,
        "source_commit": context.source_commit,
        "machine_type": spec.machine_type,
        "cells": [
            {
                "id": cell.id,
                "sender_zone": cell.sender_zone,
                "receiver_zone": cell.receiver_zone,
                "condition_profile": cell.condition_profile,
                "samples": cell.samples,
            }
            for cell in spec.cells
        ],
        "samples_total": len(runs),
        "run_ids": [run.run_id for run in runs],
        "execution_cell_order": [run.cell.id for run in runs],
        "randomization": (
            {
                "method": spec.randomization_method,
                "seed": spec.randomization_seed,
            }
            if spec.schema == MATRIX_SCHEMA_V2
            else None
        ),
        "measurement_profile": harness.MEASUREMENT_PROFILE,
        "safety": {
            "default_mode": "plan-only",
            "execute_requires_acknowledgement": True,
            "maximum_samples": MAX_MATRIX_SAMPLES,
            "balanced_complete_factorial": True,
            "cleanup": "exact run ids; no wildcard or project deletion",
        },
        "claims": {
            "calibrated_performance": False,
            "field_evidence": False,
            "causal_region_effect": False,
            "causal_condition_effect": False,
        },
    }


def _load_json(path: Path) -> dict:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise MatrixError(f"invalid evidence file: {path}") from error


def _write_json(path: Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(value, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def summarize(spec: MatrixSpec, context: RunContext) -> dict:
    validate_context(spec, context)
    runs = planned_runs(spec, context)
    timing_fields = TIMING_FIELDS + harness.FEEDBACK_RTT_TIMING_FIELDS
    metrics: dict[str, list[float]] = {
        field: [] for field in timing_fields
    }
    cell_metrics: dict[str, dict[str, list[float]]] = {
        cell.id: {field: [] for field in timing_fields}
        for cell in spec.cells
    }
    cell_run_ids: dict[str, list[str]] = {cell.id: [] for cell in spec.cells}
    reference_identity: dict | None = None

    for run in runs:
        path = run.evidence_dir / "manifest.json"
        manifest = _load_json(path)
        label = f"{run.cell.id}/sample-{run.sample_index:02d}"
        if manifest.get("schema") != harness.RAW_EVIDENCE_SCHEMA:
            raise MatrixError(f"{label}: unexpected evidence schema")
        if manifest.get("result") != "passed":
            raise MatrixError(f"{label}: transport did not pass")
        if not manifest.get("teardown", {}).get("success"):
            raise MatrixError(f"{label}: primary teardown did not pass")
        if manifest.get("run_id") != run.run_id:
            raise MatrixError(f"{label}: run id mismatch")
        if manifest.get("project") != context.project:
            raise MatrixError(f"{label}: project mismatch")
        if manifest.get("source_commit") != context.source_commit:
            raise MatrixError(f"{label}: source commit mismatch")
        if manifest.get("machine_type") != spec.machine_type:
            raise MatrixError(f"{label}: machine type mismatch")
        if manifest.get("sender", {}).get("zone") != run.cell.sender_zone:
            raise MatrixError(f"{label}: sender zone mismatch")
        if manifest.get("receiver", {}).get("zone") != run.cell.receiver_zone:
            raise MatrixError(f"{label}: receiver zone mismatch")

        profile = harness.CONDITION_PROFILES[run.cell.condition_profile]
        expected_condition = {
            "profile": profile.name,
            "forward_trace": profile.forward_name,
            "reverse_trace": profile.reverse_name,
        }
        if manifest.get("condition") != expected_condition:
            raise MatrixError(f"{label}: condition profile mismatch")
        expected_workload = {
            "id": "smoke-v2",
            "generation_count": 2,
            "startup_timeout_ms": harness.STARTUP_TIMEOUT_MS,
            "service_timeout_ms": harness.SERVICE_TIMEOUT_MS,
        }
        if manifest.get("workload") != expected_workload:
            raise MatrixError(f"{label}: workload mismatch")
        expected_measurement = harness.measurement_contract()
        if manifest.get("measurement") != expected_measurement:
            raise MatrixError(f"{label}: measurement boundary mismatch")
        claims = manifest.get("claims", {})
        if claims.get("calibrated_performance") is not False or \
                claims.get("field_evidence") is not False:
            raise MatrixError(f"{label}: evidence claims are not bounded")

        identity = {
            "topology": manifest.get("topology"),
            "runtime_binary_sha256": manifest.get("runtime_binary_sha256"),
            "authentication_profile": manifest.get("authentication_profile"),
            "workload": manifest.get("workload"),
            "measurement": manifest.get("measurement"),
        }
        if reference_identity is None:
            reference_identity = identity
        elif identity != reference_identity:
            raise MatrixError(f"{label}: mixed matrix identity")

        runtime_hash = manifest.get("runtime_binary_sha256")
        sender = manifest.get("sender", {})
        receiver = manifest.get("receiver", {})
        if sender.get("binary_sha256") != runtime_hash or \
                receiver.get("binary_sha256") != runtime_hash:
            raise MatrixError(f"{label}: host binary mismatch")
        if sender.get("ntp_synchronized") is not True or \
                receiver.get("ntp_synchronized") is not True:
            raise MatrixError(f"{label}: host NTP evidence is not synchronized")

        sender_text = (run.evidence_dir / "sender.log").read_text(
            encoding="utf-8"
        )
        receiver_text = (run.evidence_dir / "receiver.log").read_text(
            encoding="utf-8"
        )
        expected_sender_hash = hashlib.sha256(
            sender_text.encode("utf-8")
        ).hexdigest()
        expected_receiver_hash = hashlib.sha256(
            receiver_text.encode("utf-8")
        ).hexdigest()
        if manifest.get("logs", {}).get("sender_sha256") != expected_sender_hash:
            raise MatrixError(f"{label}: sender log hash mismatch")
        if manifest.get("logs", {}).get("receiver_sha256") != expected_receiver_hash:
            raise MatrixError(f"{label}: receiver log hash mismatch")
        verified = harness.verify_process_logs(sender_text, receiver_text)

        timing = manifest.get("timing", {})
        for field, value in verified.items():
            if timing.get(field) != value:
                raise MatrixError(f"{label}: log/manifest timing mismatch for {field}")
        for field in timing_fields:
            value = timing.get(field)
            if not isinstance(value, (int, float)) or value <= 0:
                raise MatrixError(f"{label}: invalid timing field {field}")
            metrics[field].append(float(value))
            cell_metrics[run.cell.id][field].append(float(value))
        cell_run_ids[run.cell.id].append(run.run_id)

    if reference_identity is None:
        raise MatrixError("matrix contains no evidence")

    cells = []
    for cell in spec.cells:
        cells.append(
            {
                "id": cell.id,
                "sender_zone": cell.sender_zone,
                "receiver_zone": cell.receiver_zone,
                "sender_region": harness.zone_region(cell.sender_zone),
                "receiver_region": harness.zone_region(cell.receiver_zone),
                "condition_profile": cell.condition_profile,
                "samples_expected": cell.samples,
                "samples_observed": len(cell_run_ids[cell.id]),
                "sample_run_ids": cell_run_ids[cell.id],
                "timing": {
                    field: metric_summary(values)
                    for field, values in cell_metrics[cell.id].items()
                },
            }
        )

    return {
        "schema": EVIDENCE_SCHEMA,
        "evidence_level": "emulation",
        "result": "passed",
        "matrix_id": spec.matrix_id,
        "project": context.project,
        "source_commit": context.source_commit,
        "machine_type": spec.machine_type,
        "topology": reference_identity["topology"],
        "runtime_binary_sha256": reference_identity["runtime_binary_sha256"],
        "authentication_profile": reference_identity[
            "authentication_profile"
        ],
        "workload": reference_identity["workload"],
        "measurement": reference_identity["measurement"],
        "cells_expected": len(spec.cells),
        "cells_observed": len(cells),
        "samples_expected": spec.total_samples,
        "samples_observed": len(runs),
        "balanced_complete_factorial": True,
        "all_primary_teardowns_passed": True,
        "cells": cells,
        "overall_timing": {
            field: metric_summary(values) for field, values in metrics.items()
        },
        "claims": {
            "calibrated_performance": False,
            "field_evidence": False,
            "causal_region_effect": False,
            "causal_condition_effect": False,
            "one_way_latency": False,
            "timing_scope": harness.TIMING_SCOPE,
        },
    }


def execute_matrix(
    spec: MatrixSpec,
    context: RunContext,
    runner: harness.CommandRunner,
    *,
    gcloud: str,
) -> dict:
    validate_context(spec, context)
    for run in planned_runs(spec, context):
        harness.GcpRawHarness(
            run_config(spec, context, run), runner, gcloud=gcloud
        ).execute()
    result = summarize(spec, context)
    _write_json(context.evidence_root / "matrix-manifest.json", result)
    return result


def cleanup_matrix(
    spec: MatrixSpec,
    context: RunContext,
    runner: harness.CommandRunner,
    *,
    gcloud: str,
) -> dict:
    validate_context(spec, context)
    records = []
    for run in planned_runs(spec, context):
        result = harness.GcpRawHarness(
            run_config(spec, context, run), runner, gcloud=gcloud
        ).cleanup()
        record = {
            "cell_id": run.cell.id,
            "sample_index": run.sample_index,
            "run_id": run.run_id,
            **result,
        }
        records.append(record)
        _write_json(run.evidence_dir / "cleanup.json", record)
    summary = {
        "schema": "aurora-gcp-raw-matrix-cleanup-v1",
        "matrix_id": spec.matrix_id,
        "project": context.project,
        "success": all(record["success"] for record in records),
        "runs_expected": spec.total_samples,
        "runs_observed": len(records),
        "records": records,
    }
    _write_json(context.evidence_root / "cleanup-manifest.json", summary)
    return summary


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--matrix", type=Path, required=True)
    result.add_argument("--project", required=True)
    result.add_argument("--run-prefix", required=True)
    result.add_argument("--source-commit", default=harness.current_commit())
    result.add_argument(
        "--repository-url",
        default="https://github.com/Daniele-Cangi/Aurora.git",
    )
    result.add_argument("--evidence-root", type=Path, required=True)
    mode = result.add_mutually_exclusive_group()
    mode.add_argument("--execute", action="store_true")
    mode.add_argument("--cleanup-only", action="store_true")
    mode.add_argument("--summarize-only", action="store_true")
    result.add_argument(
        "--acknowledge-billing-and-teardown", action="store_true"
    )
    result.add_argument("--quiet", action="store_true")
    return result


def main(argv: Iterable[str] | None = None) -> int:
    args = parser().parse_args(argv)
    spec = MatrixSpec.load(args.matrix)
    context = RunContext(
        project=args.project,
        run_prefix=args.run_prefix,
        source_commit=args.source_commit,
        repository_url=args.repository_url,
        evidence_root=args.evidence_root,
    )
    if args.summarize_only:
        result = summarize(spec, context)
        _write_json(context.evidence_root / "matrix-manifest.json", result)
        print(json.dumps(result, indent=2, sort_keys=True))
        return 0
    if not args.execute and not args.cleanup_only:
        print(json.dumps(build_plan(spec, context), indent=2, sort_keys=True))
        return 0
    if not args.acknowledge_billing_and_teardown:
        raise MatrixError(
            "execution and cleanup require --acknowledge-billing-and-teardown"
        )
    gcloud = shutil.which("gcloud")
    if not gcloud:
        raise MatrixError("gcloud was not found")
    runner = harness.CommandRunner(verbose=not args.quiet)
    if args.cleanup_only:
        result = cleanup_matrix(spec, context, runner, gcloud=gcloud)
        print(json.dumps(result, indent=2, sort_keys=True))
        return 0 if result["success"] else 1
    result = execute_matrix(spec, context, runner, gcloud=gcloud)
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (MatrixError, harness.HarnessError) as error:
        print(f"gcp raw matrix: {error}")
        raise SystemExit(1)
