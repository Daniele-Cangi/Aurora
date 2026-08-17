#!/usr/bin/env python3
"""Analyze the frozen raw-host post-shock efficiency evidence."""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import statistics
from typing import Iterable

from gcp_raw_power_analysis import T_CRITICAL_005_TWO_SIDED
from gcp_raw_post_shock_evidence import EvidenceError, sha256_file, validate_lifecycle
from gcp_raw_post_shock_study import (
    Context,
    DEFAULT_SPEC,
    Spec,
    StudyError,
    planned_runs,
)


class AnalysisError(RuntimeError):
    pass


def paired_contrasts(
    blocks: list[dict],
    extractor,
) -> list[float]:
    return [
        float(extractor(block["biological-adaptive"])) -
        float(extractor(block["fixed-class-aware"]))
        for block in blocks
    ]


def contrast_summary(values: list[float]) -> dict:
    return {
        "mean": statistics.mean(values),
        "standard_deviation": statistics.stdev(values),
        "minimum": min(values),
        "maximum": max(values),
    }


def analyze(
    spec: Spec,
    context: Context,
    frozen_evidence_archive: Path,
) -> dict:
    if not frozen_evidence_archive.is_file():
        raise AnalysisError("frozen evidence archive does not exist")
    archive_sha256 = sha256_file(frozen_evidence_archive)
    runs = planned_runs(spec, context)
    by_block: dict[int, dict[str, dict]] = {}
    common_binary = None
    lifecycle_results = []
    for run in runs:
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
            raise AnalysisError("study did not use one identical runtime binary")
        block = by_block.setdefault(run.block, {})
        if run.cell.policy in block:
            raise AnalysisError(f"duplicate treatment in block {run.block}")
        block[run.cell.policy] = result
        lifecycle_results.append({
            "sequence": run.sequence,
            "block": run.block,
            **result,
        })
    expected_policies = {"fixed-class-aware", "biological-adaptive"}
    if len(by_block) != 23 or any(
        set(block) != expected_policies for block in by_block.values()
    ):
        raise AnalysisError("evidence is not 23 complete paired blocks")
    blocks = [by_block[index] for index in range(1, 24)]

    primary = paired_contrasts(
        blocks,
        lambda run: run["post_shock"]["wire_symbol_datagrams"],
    )
    mean = statistics.mean(primary)
    standard_deviation = statistics.stdev(primary)
    standard_error = standard_deviation / math.sqrt(len(primary))
    critical = T_CRITICAL_005_TWO_SIDED[len(primary) - 1]
    lower = mean - critical * standard_error
    upper = mean + critical * standard_error
    if standard_error == 0.0:
        statistic = None
    else:
        statistic = mean / standard_error

    delivery_guardrail = all(
        run["post_shock"]["critical_before_deadline"] == 5
        for block in blocks for run in block.values()
    )
    statistical_efficiency = upper < 0.0
    minimum_relevant_effect = 50.0
    point_estimate_practically_relevant = mean <= -minimum_relevant_effect
    if not delivery_guardrail:
        classification = "delivery-guardrail-failed-efficiency-inconclusive"
    elif statistical_efficiency and point_estimate_practically_relevant:
        classification = (
            "efficiency-advantage-statistically-supported-and-practically-relevant"
        )
    elif statistical_efficiency:
        classification = "efficiency-advantage-smaller-than-registered-relevance"
    else:
        classification = "no-confirmatory-post-shock-efficiency-advantage"

    secondary_extractors = {
        "initial_symbols": lambda run: run["post_shock"]["initial_symbols"],
        "repair_symbols_requested": lambda run: run["post_shock"][
            "repair_symbols_requested"
        ],
        "repair_symbols_emitted": lambda run: run["post_shock"][
            "repair_symbols_emitted"
        ],
        "total_delivery": lambda run: run["post_shock"]["delivered"],
        "terminal_feedback_rtt_mean_us": lambda run: run["whole_lifecycle"][
            "terminal_feedback_rtt_mean_us"
        ],
        "terminal_feedback_retry_rounds": lambda run: run["whole_lifecycle"][
            "terminal_feedback_retry_rounds"
        ],
    }
    secondary = {
        name: {
            "contrast": "biological-adaptive-minus-fixed-class-aware",
            **contrast_summary(paired_contrasts(blocks, extractor)),
        }
        for name, extractor in secondary_extractors.items()
    }
    block_results = []
    for index, block in enumerate(blocks, start=1):
        fixed = block["fixed-class-aware"]["post_shock"]
        biological = block["biological-adaptive"]["post_shock"]
        block_results.append({
            "block": index,
            "fixed_class_aware_wire_symbols": fixed["wire_symbol_datagrams"],
            "biological_adaptive_wire_symbols": biological[
                "wire_symbol_datagrams"
            ],
            "primary_contrast_wire_symbols":
                biological["wire_symbol_datagrams"] -
                fixed["wire_symbol_datagrams"],
            "fixed_class_aware_critical_before_deadline": fixed[
                "critical_before_deadline"
            ],
            "biological_adaptive_critical_before_deadline": biological[
                "critical_before_deadline"
            ],
        })
    return {
        "schema": "aurora-raw-post-shock-efficiency-analysis-v1",
        "study_id": spec.value["study_id"],
        "source_commit": context.source_commit,
        "runtime_binary_sha256": common_binary,
        "frozen_evidence_archive": str(frozen_evidence_archive),
        "frozen_evidence_archive_sha256": archive_sha256,
        "complete_blocks": len(blocks),
        "lifecycles": len(lifecycle_results),
        "primary_analysis": {
            "outcome": "post-shock-wire-symbol-datagrams-over-scheduled-generations",
            "contrast": "biological-adaptive-minus-fixed-class-aware",
            "shock_generation_excluded": 2,
            "post_shock_generation_indices": [3, 4, 5, 6, 7],
            "mean_contrast_wire_symbols": mean,
            "mean_contrast_wire_symbols_per_scheduled_generation": mean / 5.0,
            "standard_deviation_wire_symbols": standard_deviation,
            "standard_error_wire_symbols": standard_error,
            "t_statistic": statistic,
            "degrees_of_freedom": len(primary) - 1,
            "critical_t_two_sided_alpha_0_05": critical,
            "confidence_interval_95_percent": [lower, upper],
            "confidence_interval_excludes_zero_in_efficiency_direction":
                statistical_efficiency,
            "minimum_relevant_effect_wire_symbols": minimum_relevant_effect,
            "point_estimate_meets_registered_relevance":
                point_estimate_practically_relevant,
        },
        "delivery_guardrail": {
            "rule": "all five post-shock critical generations before receiver deadline in both treatments and every block",
            "passed": delivery_guardrail,
        },
        "classification": classification,
        "delivery_superiority_claim": False,
        "block_results": block_results,
        "secondary_descriptive_contrasts": secondary,
        "per_lifecycle_evidence": lifecycle_results,
        "claims": spec.value["claims"],
    }


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--spec", type=Path, default=DEFAULT_SPEC)
    result.add_argument("--project", required=True)
    result.add_argument("--run-prefix", default="postshock-v1")
    result.add_argument("--source-commit", required=True)
    result.add_argument(
        "--repository-url",
        default="https://github.com/Daniele-Cangi/Aurora.git",
    )
    result.add_argument("--evidence-root", type=Path, required=True)
    result.add_argument("--frozen-evidence-archive", type=Path, required=True)
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
    print(json.dumps(
        analyze(spec, context, args.frozen_evidence_archive),
        indent=2,
        sort_keys=True,
        allow_nan=False,
    ))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AnalysisError, EvidenceError, StudyError) as error:
        print(f"post-shock study analysis: {error}")
        raise SystemExit(1)
