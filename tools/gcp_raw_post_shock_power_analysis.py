#!/usr/bin/env python3
"""Reproduce the frozen post-shock efficiency study power calculation."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
from typing import Iterable

from gcp_raw_power_analysis import paired_t_power


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_STUDY = (
    ROOT / "benchmarks" / "gcp_raw_post_shock_efficiency_study_v1.json"
)
DEFAULT_PLANNING_INPUT = (
    ROOT / "benchmarks" /
    "raw_host_policy_pilot_v1_efficiency_planning_input.json"
)


class PostShockPowerError(RuntimeError):
    pass


def load_json(path: Path) -> dict:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise PostShockPowerError(f"invalid JSON: {path}") from error
    if not isinstance(value, dict):
        raise PostShockPowerError(f"expected JSON object: {path}")
    return value


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(block)
    except OSError as error:
        raise PostShockPowerError(f"cannot hash: {path}") from error
    return digest.hexdigest()


def evaluate(
    study_path: Path = DEFAULT_STUDY,
    planning_input_path: Path = DEFAULT_PLANNING_INPUT,
) -> dict:
    study = load_json(study_path)
    planning = load_json(planning_input_path)
    if study.get("schema") != \
            "aurora-gcp-raw-post-shock-efficiency-study-plan-v1":
        raise PostShockPowerError("unexpected study schema")
    provenance = study.get("pilot_provenance", {})
    if provenance.get("planning_input_path") != str(
        planning_input_path.resolve().relative_to(ROOT).as_posix()
    ) or provenance.get("planning_input_sha256") != sha256_file(
        planning_input_path
    ):
        raise PostShockPowerError("planning input provenance mismatch")
    if planning.get("schema") != \
            "aurora-raw-host-policy-pilot-efficiency-planning-input-v1" or \
            planning.get("evidence_archive_sha256") != \
            "bacad78c1c678c64b8c9c605186969d54db33904ef8a347d249b12b1a4112c99":
        raise PostShockPowerError("unexpected pilot planning input")

    rows = planning.get("blocks")
    if not isinstance(rows, list) or len(rows) != 2:
        raise PostShockPowerError("planning input must contain two pilot blocks")
    contrasts = []
    for expected_block, row in enumerate(rows, start=1):
        try:
            fixed = int(row["fixed_class_aware_wire_symbols"])
            biological = int(row["biological_adaptive_wire_symbols"])
            contrast = int(row["contrast_wire_symbols"])
        except (KeyError, TypeError, ValueError) as error:
            raise PostShockPowerError("invalid pilot planning row") from error
        if row.get("block") != expected_block or biological - fixed != contrast:
            raise PostShockPowerError("pilot paired contrast is inconsistent")
        contrasts.append(contrast)
    observed_range = max(contrasts) - min(contrasts)
    planning_sd = int(math.ceil(observed_range / 10.0) * 10)
    if planning.get("planning_standard_deviation_rule") != \
            "ceil-to-next-10-of-full-pilot-contrast-range" or \
            planning.get("planning_standard_deviation_wire_symbols") != \
            planning_sd:
        raise PostShockPowerError("pilot dispersion rule is inconsistent")

    power = study.get("power", {})
    if power.get("test") != \
            "two-sided-one-sample-t-on-paired-block-contrasts" or \
            power.get("alpha") != 0.05 or \
            power.get("target_power") != 0.90 or \
            power.get("target_power_acceptance") != \
            "one-sided-95-percent-Wilson-lower-bound" or \
            power.get("planning_standard_deviation_wire_symbols") != planning_sd:
        raise PostShockPowerError("unsupported registered power design")
    try:
        effect = float(power["minimum_relevant_effect_wire_symbols"])
        trials = int(power["monte_carlo_trials_per_candidate"])
        seed = int(power["monte_carlo_seed"])
        candidates = [int(value) for value in power["candidate_blocks"]]
        registered_blocks = int(power["required_complete_blocks"])
    except (KeyError, TypeError, ValueError) as error:
        raise PostShockPowerError("invalid registered power parameters") from error
    if effect != 50.0 or candidates != list(range(20, 26)):
        raise PostShockPowerError("minimum effect or candidate set changed")

    simulations = [
        paired_t_power(
            blocks=blocks,
            effect_us=effect,
            standard_deviation_us=float(planning_sd),
            trials=trials,
            seed=seed + blocks,
        )
        for blocks in candidates
    ]
    qualifying = [
        row["blocks"] for row in simulations
        if row["one_sided_95_percent_wilson_lower"] >= power["target_power"]
    ]
    if not qualifying:
        raise PostShockPowerError("no registered candidate meets target power")
    selected = min(qualifying)
    if selected != registered_blocks:
        raise PostShockPowerError(
            f"registered blocks {registered_blocks} != reproduced {selected}"
        )
    return {
        "schema": "aurora-raw-post-shock-power-analysis-v1",
        "study_id": study.get("study_id"),
        "pilot_contrasts_wire_symbols": contrasts,
        "pilot_contrast_range_wire_symbols": observed_range,
        "planning_standard_deviation_wire_symbols": planning_sd,
        "minimum_relevant_effect_wire_symbols": effect,
        "simulations": simulations,
        "required_complete_blocks": selected,
        "treatment_lifecycles_per_block": 2,
        "maximum_lifecycles": selected * 2,
    }


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--study", type=Path, default=DEFAULT_STUDY)
    result.add_argument(
        "--planning-input", type=Path, default=DEFAULT_PLANNING_INPUT
    )
    return result


def main(argv: Iterable[str] | None = None) -> int:
    args = parser().parse_args(argv)
    print(json.dumps(
        evaluate(args.study, args.planning_input),
        indent=2,
        sort_keys=True,
    ))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except PostShockPowerError as error:
        print(f"post-shock power analysis: {error}")
        raise SystemExit(1)
