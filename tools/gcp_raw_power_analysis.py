#!/usr/bin/env python3
"""Validate the pre-registered powered GCP condition study.

The calculation deliberately uses only Python's standard library. Prospective
power is estimated with a fixed-seed Monte Carlo simulation of the declared
paired t test. The pilot is used to reconstruct block contrasts, while the
study design supplies a deliberately inflated standard deviation.
"""

from __future__ import annotations

import argparse
import csv
import io
import json
import math
from pathlib import Path
import random
import statistics


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_STUDY = ROOT / "benchmarks" / "gcp_raw_powered_condition_study_v3.json"
DEFAULT_PILOT_PLAN = ROOT / "benchmarks" / "gcp_raw_measurement_pilot_v2.json"
DEFAULT_PILOT_EVIDENCE = (
    ROOT / "benchmarks" / "raw_public_host_measurement_pilot_v2.txt"
)

# Two-sided alpha=0.05 Student t critical values. The registered design uses
# df=5..7, but the wider table keeps the implementation useful for bounded
# sensitivity checks without adding scipy as a project dependency.
T_CRITICAL_005_TWO_SIDED = {
    1: 12.706205,
    2: 4.302653,
    3: 3.182446,
    4: 2.776445,
    5: 2.570582,
    6: 2.446912,
    7: 2.364624,
    8: 2.306004,
    9: 2.262157,
    10: 2.228139,
    11: 2.200985,
    12: 2.178813,
    13: 2.160369,
    14: 2.144787,
    15: 2.131450,
    16: 2.119905,
    17: 2.109816,
    18: 2.100922,
    19: 2.093024,
    20: 2.085963,
    21: 2.079614,
    22: 2.073873,
    23: 2.068658,
    24: 2.063899,
    25: 2.059539,
    26: 2.055529,
    27: 2.051831,
    28: 2.048407,
    29: 2.045230,
    30: 2.042272,
}


class PowerAnalysisError(RuntimeError):
    pass


def load_json(path: Path) -> dict:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise PowerAnalysisError(f"invalid JSON file: {path}") from error
    if not isinstance(value, dict):
        raise PowerAnalysisError(f"expected a JSON object: {path}")
    return value


def evidence_sections(path: Path) -> dict[str, list[str]]:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        raise PowerAnalysisError(f"cannot read pilot evidence: {path}") from error
    result: dict[str, list[str]] = {}
    current: str | None = None
    for line in lines:
        if line.startswith("[") and line.endswith("]"):
            current = line[1:-1]
            result[current] = []
        elif current is not None and line:
            result[current].append(line)
    return result


def csv_section(sections: dict[str, list[str]], name: str) -> list[dict[str, str]]:
    lines = sections.get(name)
    if not lines:
        raise PowerAnalysisError(f"missing evidence section: {name}")
    return list(csv.DictReader(io.StringIO("\n".join(lines))))


def pilot_block_contrasts(
    pilot_plan_path: Path = DEFAULT_PILOT_PLAN,
    pilot_evidence_path: Path = DEFAULT_PILOT_EVIDENCE,
) -> list[float]:
    plan = load_json(pilot_plan_path)
    sections = evidence_sections(pilot_evidence_path)
    order_rows = csv_section(sections, "execution_order")
    sample_rows = csv_section(sections, "sample_same_clock_timing")
    primary_metric = plan.get("analysis", {}).get("primary_sender_clock_metric")
    if primary_metric != "terminal_feedback_rtt_mean_us":
        raise PowerAnalysisError("unexpected pilot primary metric")

    cells = plan.get("cells")
    if not isinstance(cells, list) or len(cells) != 4:
        raise PowerAnalysisError("pilot must declare four cells")
    cell_factors = {}
    for cell in cells:
        try:
            pair = (cell["sender_zone"], cell["receiver_zone"])
            cell_factors[cell["id"]] = (pair, cell["condition_profile"])
        except (KeyError, TypeError) as error:
            raise PowerAnalysisError("invalid pilot cell") from error

    samples = {}
    for row in sample_rows:
        try:
            samples[row["run_id"]] = float(row[primary_metric])
        except (KeyError, ValueError) as error:
            raise PowerAnalysisError("invalid pilot sample timing") from error

    blocks: dict[int, dict[str, float]] = {}
    for row in order_rows:
        try:
            block = int(row["block"])
            cell = row["cell"]
            run_id = row["run_id"]
            value = samples[run_id]
        except (KeyError, ValueError) as error:
            raise PowerAnalysisError("invalid pilot execution order") from error
        if cell not in cell_factors:
            raise PowerAnalysisError(f"unknown pilot cell: {cell}")
        block_values = blocks.setdefault(block, {})
        if cell in block_values:
            raise PowerAnalysisError(f"duplicate cell in pilot block {block}")
        block_values[cell] = value

    result = []
    expected_cells = set(cell_factors)
    for block in sorted(blocks):
        values = blocks[block]
        if set(values) != expected_cells:
            raise PowerAnalysisError(f"incomplete pilot block {block}")
        pair_contrasts = []
        pairs = {factors[0] for factors in cell_factors.values()}
        for pair in sorted(pairs):
            timed = [
                values[cell]
                for cell, factors in cell_factors.items()
                if factors == (pair, "timed-replay-v2")
            ]
            zero = [
                values[cell]
                for cell, factors in cell_factors.items()
                if factors == (pair, "zero-delay-replay-v1")
            ]
            if len(timed) != 1 or len(zero) != 1:
                raise PowerAnalysisError("pilot factors are not a complete 2x2")
            pair_contrasts.append(timed[0] - zero[0])
        result.append(statistics.mean(pair_contrasts))
    if len(result) < 2:
        raise PowerAnalysisError("at least two pilot blocks are required")
    return result


def wilson_lower(successes: int, trials: int, z: float) -> float:
    if not 0 <= successes <= trials or trials <= 0:
        raise PowerAnalysisError("invalid Monte Carlo counts")
    p = successes / trials
    denominator = 1.0 + z * z / trials
    centre = p + z * z / (2.0 * trials)
    radius = z * math.sqrt(p * (1.0 - p) / trials + z * z / (4.0 * trials * trials))
    return (centre - radius) / denominator


def paired_t_power(
    *,
    blocks: int,
    effect_us: float,
    standard_deviation_us: float,
    trials: int,
    seed: int,
) -> dict:
    if blocks < 2 or blocks - 1 not in T_CRITICAL_005_TWO_SIDED:
        raise PowerAnalysisError("blocks must be in 2..31")
    if effect_us <= 0 or standard_deviation_us <= 0:
        raise PowerAnalysisError("effect and standard deviation must be positive")
    if trials < 1000:
        raise PowerAnalysisError("at least 1000 Monte Carlo trials are required")

    rng = random.Random(seed)
    critical = T_CRITICAL_005_TWO_SIDED[blocks - 1]
    successes = 0
    for _ in range(trials):
        total = 0.0
        total_squares = 0.0
        for _ in range(blocks):
            value = rng.gauss(effect_us, standard_deviation_us)
            total += value
            total_squares += value * value
        mean = total / blocks
        variance = (total_squares - total * total / blocks) / (blocks - 1)
        sample_sd = math.sqrt(max(variance, 0.0))
        if sample_sd == 0.0:
            successes += mean != 0.0
            continue
        statistic = mean / (sample_sd / math.sqrt(blocks))
        successes += abs(statistic) >= critical

    estimate = successes / trials
    return {
        "blocks": blocks,
        "degrees_of_freedom": blocks - 1,
        "critical_t_two_sided_alpha_0_05": critical,
        "successes": successes,
        "trials": trials,
        "estimated_power": round(estimate, 6),
        "one_sided_95_percent_wilson_lower": round(
            wilson_lower(successes, trials, 1.644854), 6
        ),
    }


def evaluate_study(
    study_path: Path = DEFAULT_STUDY,
    pilot_plan_path: Path = DEFAULT_PILOT_PLAN,
    pilot_evidence_path: Path = DEFAULT_PILOT_EVIDENCE,
) -> dict:
    study = load_json(study_path)
    if study.get("schema") != "aurora-gcp-powered-condition-study-v3":
        raise PowerAnalysisError("unexpected study schema")
    power = study.get("power")
    if not isinstance(power, dict):
        raise PowerAnalysisError("study power section is missing")
    if power.get("test") != "two-sided-one-sample-t-on-block-contrasts":
        raise PowerAnalysisError("unexpected registered test")
    if power.get("alpha") != 0.05:
        raise PowerAnalysisError("only registered alpha=0.05 is supported")

    contrasts = pilot_block_contrasts(pilot_plan_path, pilot_evidence_path)
    observed_sd = statistics.stdev(contrasts)
    # With three pilot blocks, df=2 and chi-square(0.05, 2) has the closed form
    # -2*ln(0.95). This is the one-sided 95% upper confidence bound for sigma.
    variance_upper = observed_sd * math.sqrt(
        (len(contrasts) - 1) / (-2.0 * math.log(0.95))
    )

    try:
        effect = float(power["minimum_relevant_effect_us"])
        design_sd = float(power["design_standard_deviation_us"])
        target = float(power["target_power"])
        trials = int(power["monte_carlo_trials"])
        seed = int(power["monte_carlo_seed"])
        candidates = [int(value) for value in power["candidate_blocks"]]
    except (KeyError, TypeError, ValueError) as error:
        raise PowerAnalysisError("invalid study power inputs") from error
    if candidates != sorted(set(candidates)):
        raise PowerAnalysisError("candidate blocks must be unique and sorted")
    if design_sd < variance_upper:
        raise PowerAnalysisError("design standard deviation is below pilot upper bound")

    simulations = [
        paired_t_power(
            blocks=blocks,
            effect_us=effect,
            standard_deviation_us=design_sd,
            trials=trials,
            seed=seed + blocks,
        )
        for blocks in candidates
    ]
    qualifying = [
        item["blocks"]
        for item in simulations
        if item["one_sided_95_percent_wilson_lower"] >= target
    ]
    if not qualifying:
        raise PowerAnalysisError("no candidate reaches the registered power target")
    required = min(qualifying)
    if required != power.get("blocks_required"):
        raise PowerAnalysisError("registered block count is not the first powered candidate")

    campaigns = study.get("campaigns")
    if not isinstance(campaigns, list):
        raise PowerAnalysisError("study campaigns are missing")
    if sum(int(item.get("blocks", 0)) for item in campaigns) != required:
        raise PowerAnalysisError("campaign blocks do not match required blocks")
    if sum(int(item.get("lifecycles", 0)) for item in campaigns) != required * 4:
        raise PowerAnalysisError("campaign lifecycles do not match the 2x2 design")

    return {
        "schema": "aurora-gcp-powered-condition-power-report-v3",
        "study_id": study.get("study_id"),
        "pilot": {
            "block_contrasts_us": contrasts,
            "mean_us": round(statistics.mean(contrasts), 6),
            "sample_standard_deviation_us": round(observed_sd, 6),
            "one_sided_95_percent_sigma_upper_us": round(variance_upper, 6),
        },
        "design": {
            "minimum_relevant_effect_us": effect,
            "design_standard_deviation_us": design_sd,
            "target_power": target,
            "required_blocks": required,
            "total_lifecycles": required * 4,
        },
        "simulations": simulations,
        "result": "passed",
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--study", type=Path, default=DEFAULT_STUDY)
    parser.add_argument("--pilot-plan", type=Path, default=DEFAULT_PILOT_PLAN)
    parser.add_argument(
        "--pilot-evidence", type=Path, default=DEFAULT_PILOT_EVIDENCE
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    report = evaluate_study(args.study, args.pilot_plan, args.pilot_evidence)
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
