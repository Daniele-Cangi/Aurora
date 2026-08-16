#!/usr/bin/env python3
"""Validate and summarize a controlled Aurora GCP raw-host campaign."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import statistics
from typing import Iterable


TIMING_FIELDS = (
    "sender_elapsed_ms",
    "receiver_service_elapsed_ms",
    "controller_receiver_ready_ms",
    "controller_sender_wall_ms",
    "controller_total_wall_ms",
)
FEEDBACK_RTT_TIMING_FIELDS = (
    "feedback_rtt_min_us",
    "feedback_rtt_mean_us",
    "feedback_rtt_max_us",
    "terminal_feedback_rtt_min_us",
    "terminal_feedback_rtt_mean_us",
    "terminal_feedback_rtt_max_us",
)
LEGACY_EVIDENCE_SCHEMA = "aurora-raw-host-evidence-v2"
FEEDBACK_EVIDENCE_SCHEMA = "aurora-raw-host-evidence-v3"
CURRENT_EVIDENCE_SCHEMA = "aurora-raw-host-evidence-v4"


class CampaignError(RuntimeError):
    pass


def metric_summary(values: list[float]) -> dict[str, float | int]:
    if not values:
        raise CampaignError("cannot summarize an empty metric")
    return {
        "count": len(values),
        "min": min(values),
        "max": max(values),
        "mean": round(statistics.fmean(values), 3),
        "median": round(statistics.median(values), 3),
        "sample_stddev": (
            round(statistics.stdev(values), 3) if len(values) > 1 else 0.0
        ),
    }


def load_manifests(evidence_root: Path) -> list[dict]:
    manifests = []
    for path in sorted(evidence_root.glob("sample-*/manifest.json")):
        try:
            value = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as error:
            raise CampaignError(f"invalid sample manifest: {path}") from error
        value["_campaign_path"] = path.parent.name
        manifests.append(value)
    return manifests


def summarize(evidence_root: Path, expected_samples: int) -> dict:
    if expected_samples <= 0:
        raise CampaignError("expected sample count must be positive")
    manifests = load_manifests(evidence_root)
    if len(manifests) != expected_samples:
        raise CampaignError(
            f"expected {expected_samples} manifests, found {len(manifests)}"
        )

    reference = manifests[0]
    identity_fields = (
        "schema",
        "project",
        "source_commit",
        "topology",
        "runtime_binary_sha256",
        "authentication_profile",
        "measurement",
    )
    run_ids: list[str] = []
    schema = reference.get("schema")
    if schema not in (
        LEGACY_EVIDENCE_SCHEMA,
        FEEDBACK_EVIDENCE_SCHEMA,
        CURRENT_EVIDENCE_SCHEMA,
    ):
        raise CampaignError("unexpected evidence schema")
    if schema == CURRENT_EVIDENCE_SCHEMA:
        identity_fields += (
            "process_protocol_version",
            "terminal_handshake",
        )
    timing_fields = TIMING_FIELDS + (
        FEEDBACK_RTT_TIMING_FIELDS
        if schema in (FEEDBACK_EVIDENCE_SCHEMA, CURRENT_EVIDENCE_SCHEMA)
        else ()
    )
    metrics: dict[str, list[float]] = {
        field: [] for field in timing_fields
    }
    for manifest in manifests:
        path = manifest["_campaign_path"]
        if manifest.get("schema") != schema:
            raise CampaignError(f"{path}: mixed evidence schema")
        if manifest.get("result") != "passed":
            raise CampaignError(f"{path}: sample did not pass")
        if not manifest.get("teardown", {}).get("success"):
            raise CampaignError(f"{path}: teardown audit did not pass")
        for field in identity_fields:
            if manifest.get(field) != reference.get(field):
                raise CampaignError(f"{path}: mixed campaign identity for {field}")
        run_id = manifest.get("run_id")
        if not isinstance(run_id, str) or not run_id:
            raise CampaignError(f"{path}: missing run id")
        run_ids.append(run_id)
        timing = manifest.get("timing", {})
        for field in timing_fields:
            value = timing.get(field)
            if not isinstance(value, (int, float)) or value <= 0:
                raise CampaignError(f"{path}: invalid timing field {field}")
            metrics[field].append(float(value))
    if len(set(run_ids)) != len(run_ids):
        raise CampaignError("campaign run ids are not unique")

    return {
        "schema": (
            "aurora-raw-host-campaign-v2"
            if schema in (FEEDBACK_EVIDENCE_SCHEMA, CURRENT_EVIDENCE_SCHEMA)
            else "aurora-raw-host-campaign-v1"
        ),
        "evidence_level": "emulation",
        "result": "passed",
        "samples_expected": expected_samples,
        "samples_observed": len(manifests),
        "project": reference["project"],
        "source_commit": reference["source_commit"],
        "topology": reference["topology"],
        "runtime_binary_sha256": reference["runtime_binary_sha256"],
        "authentication_profile": reference["authentication_profile"],
        **(
            {
                "process_protocol_version": reference[
                    "process_protocol_version"
                ],
                "terminal_handshake": reference["terminal_handshake"],
            }
            if schema == CURRENT_EVIDENCE_SCHEMA else {}
        ),
        "sample_run_ids": run_ids,
        "all_primary_teardowns_passed": True,
        "timing": {
            field: metric_summary(values) for field, values in metrics.items()
        },
        "claims": {
            "calibrated_performance": False,
            "field_evidence": False,
            "timing_scope": (
                "application-controller-and-sender-feedback-steady-clocks"
                if schema in (FEEDBACK_EVIDENCE_SCHEMA, CURRENT_EVIDENCE_SCHEMA)
                else "application-and-controller-steady-clock"
            ),
            "one_way_latency": False,
        },
    }


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--evidence-root", type=Path, required=True)
    result.add_argument("--expected-samples", type=int, required=True)
    result.add_argument("--output", type=Path)
    return result


def main(argv: Iterable[str] | None = None) -> int:
    args = parser().parse_args(argv)
    summary = summarize(args.evidence_root, args.expected_samples)
    output = args.output or args.evidence_root / "campaign-manifest.json"
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(json.dumps(summary, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except CampaignError as error:
        print(f"gcp raw campaign: {error}")
        raise SystemExit(1)
