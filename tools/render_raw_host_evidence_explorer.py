#!/usr/bin/env python3
"""Render the standalone Aurora raw-host pilot evidence explorer."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import re
from typing import Iterable


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_INPUT = (
    ROOT / "benchmarks" / "raw_host_policy_pilot_v1_explorer_input.json"
)
DEFAULT_OUTPUT = ROOT / "docs" / "raw-host-policy-pilot-v1-explorer.html"
SCHEMA = "aurora-raw-host-policy-pilot-evidence-explorer-input-v1"
POLICIES = (
    "fixed-minimum",
    "fixed-class-aware",
    "biological-adaptive",
)
CONDITIONS = ("timed-replay-v2", "regime-change-v1")
SHA256_PATTERN = re.compile(r"^[0-9a-f]{64}$")
COMMIT_PATTERN = re.compile(r"^[0-9a-f]{40}$")


class ExplorerError(RuntimeError):
    pass


def load_json(path: Path) -> dict:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ExplorerError(f"invalid JSON input: {path}") from error
    if not isinstance(value, dict):
        raise ExplorerError(f"JSON input is not an object: {path}")
    return value


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(block)
    except OSError as error:
        raise ExplorerError(f"cannot hash file: {path}") from error
    return digest.hexdigest()


def write_lf(path: Path, value: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    try:
        with path.open("w", encoding="utf-8", newline="\n") as stream:
            stream.write(value.rstrip() + "\n")
    except OSError as error:
        raise ExplorerError(f"cannot write explorer: {path}") from error


def require_object(value: object, label: str) -> dict:
    if not isinstance(value, dict):
        raise ExplorerError(f"{label} must be an object")
    return value


def require_list(value: object, label: str, length: int | None = None) -> list:
    if not isinstance(value, list):
        raise ExplorerError(f"{label} must be an array")
    if length is not None and len(value) != length:
        raise ExplorerError(f"{label} must contain {length} values")
    return value


def require_number(value: object, label: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ExplorerError(f"{label} must be numeric")
    result = float(value)
    if not math.isfinite(result):
        raise ExplorerError(f"{label} must be finite")
    return result


def require_nonnegative(value: object, label: str) -> float:
    result = require_number(value, label)
    if result < 0:
        raise ExplorerError(f"{label} must not be negative")
    return result


def validate_state(state: object, label: str) -> None:
    values = require_object(state, label)
    if set(values) != {"success", "failure", "panic"}:
        raise ExplorerError(f"{label} has unexpected counters")
    for key, value in values.items():
        number = require_nonnegative(value, f"{label}.{key}")
        if not number.is_integer():
            raise ExplorerError(f"{label}.{key} must be an integer")


def validate_input(value: dict) -> None:
    if value.get("schema") != SCHEMA:
        raise ExplorerError("unexpected explorer input schema")
    if value.get("mode") != "descriptive-policy-discrimination":
        raise ExplorerError("explorer input must remain descriptive")

    provenance = require_object(value.get("provenance"), "provenance")
    for key in (
        "runtime_binary_sha256",
        "evidence_archive_sha256",
        "analysis_json_sha256",
    ):
        if not SHA256_PATTERN.fullmatch(str(provenance.get(key, ""))):
            raise ExplorerError(f"provenance.{key} is not a SHA-256")
    for key in ("experimental_source", "controller_commit"):
        if not COMMIT_PATTERN.fullmatch(str(provenance.get(key, ""))):
            raise ExplorerError(f"provenance.{key} is not a commit id")

    integrity = require_object(value.get("integrity"), "integrity")
    if integrity.get("planned_lifecycles") != 12 or \
            integrity.get("valid_lifecycles") != 12:
        raise ExplorerError("pilot lifecycle count must remain 12/12")
    if integrity.get("replacement_lifecycles") != 0:
        raise ExplorerError("replacement lifecycles must remain zero")
    if integrity.get("randomized_complete_blocks") != 2:
        raise ExplorerError("pilot block count must remain two")
    for key in (
        "identical_binary_across_treatments",
        "integrated_teardown_audits_zero",
        "independent_post_collection_audit_zero",
    ):
        if integrity.get(key) is not True:
            raise ExplorerError(f"integrity.{key} must be true")

    deadline = require_object(
        value.get("deadline_semantics"), "deadline_semantics"
    )
    if deadline.get("clock") != "receiver-steady-descriptor-relative":
        raise ExplorerError("deadline clock must remain receiver-local")
    if deadline.get("cross_clock_subtraction_permitted") is not False or \
            deadline.get("sender_relative_expiry_references") != 0 or \
            deadline.get("violations") != 0:
        raise ExplorerError("deadline evidence contains a clock violation")

    windows = require_object(value.get("windows"), "windows")
    if windows.get("pre_shock_generation_indices") != [0, 1] or \
            windows.get("imposed_shock_generation_index") != 2 or \
            windows.get("post_shock_generation_indices") != [3, 4, 5, 6, 7]:
        raise ExplorerError("generation windows differ from the frozen pilot")

    policies = require_object(value.get("policies"), "policies")
    if set(policies) != set(POLICIES):
        raise ExplorerError("explorer must contain exactly the three policies")
    for policy in POLICIES:
        item = require_object(policies[policy], f"policies.{policy}")
        factors = require_list(
            item.get("protection_factors"),
            f"policies.{policy}.protection_factors",
            8,
        )
        for generation, row in enumerate(factors):
            values = require_list(
                row,
                f"policies.{policy}.protection_factors[{generation}]",
                3,
            )
            for index, factor in enumerate(values):
                if require_number(
                    factor,
                    f"policies.{policy}.protection_factors[{generation}]"
                    f"[{index}]",
                ) <= 0:
                    raise ExplorerError("protection factors must be positive")
        state = item.get("adaptive_state")
        if policy != "biological-adaptive":
            if item.get("kind") != "fixed" or state is not None:
                raise ExplorerError(f"{policy} must remain fixed and stateless")
            if len({tuple(row) for row in factors}) != 1:
                raise ExplorerError(f"{policy} protection factors changed")
        else:
            if item.get("kind") != "adaptive":
                raise ExplorerError("biological policy must remain adaptive")
            states = require_list(state, "biological adaptive_state", 8)
            for generation, entry in enumerate(states):
                item_state = require_object(
                    entry, f"biological adaptive_state[{generation}]"
                )
                if item_state.get("generation") != generation:
                    raise ExplorerError("adaptive state generation is out of order")
                validate_state(
                    item_state.get("at_plan"),
                    f"biological adaptive_state[{generation}].at_plan",
                )
            shock = states[2]
            if shock.get("terminal_outcome") != "imposed-failure":
                raise ExplorerError("generation 2 must be the imposed failure")
            validate_state(shock.get("after_terminal"), "shock.after_terminal")
            if factors[3] == factors[2]:
                raise ExplorerError(
                    "first biological plan after failure did not change"
                )

    delivery = require_object(value.get("delivery"), "delivery")
    if set(delivery) != set(CONDITIONS):
        raise ExplorerError("delivery conditions differ from the pilot")
    for condition in CONDITIONS:
        cells = require_object(delivery[condition], f"delivery.{condition}")
        if set(cells) != set(POLICIES):
            raise ExplorerError(f"delivery.{condition} lacks a policy")
        expected = "8/8" if condition == "timed-replay-v2" else "7/8"
        for policy in POLICIES:
            if cells[policy] != [expected, expected]:
                raise ExplorerError(
                    f"delivery.{condition}.{policy} changed from frozen result"
                )

    means = require_list(
        value.get("condition_means_across_two_blocks"),
        "condition_means_across_two_blocks",
        6,
    )
    seen_cells = set()
    for index, cell_value in enumerate(means):
        cell = require_object(
            cell_value, f"condition_means_across_two_blocks[{index}]"
        )
        key = (cell.get("condition"), cell.get("policy"))
        if key[0] not in CONDITIONS or key[1] not in POLICIES or key in seen_cells:
            raise ExplorerError("condition mean cells are invalid or duplicated")
        seen_cells.add(key)
        for metric in (
            "initial_symbols",
            "repair_requested",
            "repair_emitted",
            "wire_symbols",
            "terminal_feedback_rtt_mean_us",
            "terminal_feedback_retry_rounds",
        ):
            require_nonnegative(cell.get(metric), f"cell {key}.{metric}")

    post_shock = require_object(value.get("post_shock"), "post_shock")
    if post_shock.get("generation_indices") != [3, 4, 5, 6, 7] or \
            post_shock.get("scheduled_generations") != 5 or \
            post_shock.get("source_symbols_per_lifecycle") != 200:
        raise ExplorerError("post-shock denominator differs from frozen result")
    post_policies = require_object(post_shock.get("policies"), "post_shock.policies")
    if set(post_policies) != set(POLICIES):
        raise ExplorerError("post-shock evidence lacks a policy")
    for policy in POLICIES:
        evidence = require_object(post_policies[policy], f"post_shock.{policy}")
        for metric in (
            "critical_before_deadline",
            "initial_symbols",
            "repair_requested",
            "repair_emitted",
            "wire_symbols",
        ):
            values = require_list(evidence.get(metric), f"post_shock.{policy}.{metric}", 2)
            for block, metric_value in enumerate(values, start=1):
                require_nonnegative(metric_value, f"post_shock.{policy}.{metric}[{block}]")
        for block in range(2):
            if evidence["critical_before_deadline"][block] != 5:
                raise ExplorerError("post-shock critical delivery must remain 5/5")
            if evidence["wire_symbols"][block] != \
                    evidence["initial_symbols"][block] + \
                    evidence["repair_emitted"][block]:
                raise ExplorerError("wire total does not equal initial plus repair")

    claims = require_object(value.get("claims"), "claims")
    if claims.get("descriptive") is not True:
        raise ExplorerError("explorer evidence must be descriptive")
    for forbidden in (
        "confirmatory",
        "policy_superiority",
        "delivery_advantage",
        "field_evidence",
        "network_only_feedback_rtt",
        "one_way_latency",
        "shock_generation_is_treatment_outcome",
    ):
        if claims.get(forbidden) is not False:
            raise ExplorerError(f"claims.{forbidden} must remain false")


def format_copy_number(value: float) -> str:
    if math.isclose(value, round(value), abs_tol=1e-9):
        return str(int(round(value)))
    return f"{value:.2f}".rstrip("0").rstrip(".")


def wire_comparison(biological: float, fixed: float) -> str:
    delta = biological - fixed
    if math.isclose(delta, 0.0, abs_tol=1e-9):
        return "the wire totals tied"
    direction = "fewer" if delta < 0 else "more"
    return (
        f"biological used {format_copy_number(abs(delta))} {direction} "
        "wire symbols"
    )


def post_shock_comparisons(value: dict) -> dict[str, str]:
    policies = value["post_shock"]["policies"]
    fixed = policies["fixed-class-aware"]["wire_symbols"]
    biological = policies["biological-adaptive"]["wire_symbols"]
    return {
        "mean": wire_comparison(
            sum(biological) / len(biological),
            sum(fixed) / len(fixed),
        ),
        **{
            str(index): wire_comparison(biological[index], fixed[index])
            for index in range(len(fixed))
        },
    }


def render_explorer(value: dict, input_sha256: str) -> str:
    payload = json.dumps(
        {
            "input_sha256": input_sha256,
            "evidence": value,
            "copy": {
                "post_shock_comparisons": post_shock_comparisons(value),
            },
        },
        ensure_ascii=False,
        separators=(",", ":"),
        allow_nan=False,
    ).replace("<", "\\u003c")
    return TEMPLATE.replace("__EVIDENCE_PAYLOAD__", payload)


TEMPLATE = r'''<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <meta name="color-scheme" content="dark light">
  <title>Aurora Evidence Explorer · Raw-host policy pilot v1</title>
  <style>
    :root {
      color-scheme: dark;
      --bg: #071018;
      --surface: #0d1924;
      --surface-2: #122231;
      --surface-3: #172b3c;
      --text: #edf6ff;
      --muted: #9bb0c2;
      --border: #274052;
      --grid: #294454;
      --fixed-minimum: #9aa9b6;
      --fixed-class: #59a7ff;
      --biological: #ff8a4c;
      --initial: #8b7dff;
      --repair: #42d3a2;
      --shock: #ff5f73;
      --good: #43d39e;
      --focus: #a8d4ff;
      --shadow: 0 18px 50px rgba(0, 0, 0, 0.24);
    }
    @media (prefers-color-scheme: light) {
      :root {
        color-scheme: light;
        --bg: #edf4f8;
        --surface: #ffffff;
        --surface-2: #f4f8fb;
        --surface-3: #e8f0f5;
        --text: #132431;
        --muted: #536b7b;
        --border: #c9d8e2;
        --grid: #d7e2e9;
        --fixed-minimum: #657787;
        --fixed-class: #1267c4;
        --biological: #c95116;
        --initial: #6254ca;
        --repair: #087f61;
        --shock: #c92942;
        --good: #087f61;
        --focus: #075ea9;
        --shadow: 0 16px 45px rgba(45, 73, 90, 0.12);
      }
    }
    * { box-sizing: border-box; }
    html { scroll-behavior: smooth; }
    body {
      margin: 0;
      background:
        radial-gradient(circle at 78% -12%, color-mix(in srgb, var(--fixed-class) 18%, transparent), transparent 35rem),
        radial-gradient(circle at -8% 24%, color-mix(in srgb, var(--biological) 12%, transparent), transparent 30rem),
        var(--bg);
      color: var(--text);
      font-family: Inter, ui-sans-serif, system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
      line-height: 1.5;
    }
    button, select { font: inherit; }
    button:focus-visible, a:focus-visible, select:focus-visible {
      outline: 3px solid var(--focus);
      outline-offset: 3px;
    }
    a { color: var(--fixed-class); }
    .skip-link {
      position: absolute;
      left: 1rem;
      top: -5rem;
      z-index: 10;
      background: var(--surface);
      color: var(--text);
      padding: .7rem 1rem;
      border-radius: .5rem;
    }
    .skip-link:focus { top: 1rem; }
    .shell { width: min(1120px, calc(100% - 2rem)); margin: 0 auto; }
    .hero { padding: 3.75rem 0 2rem; }
    .eyebrow {
      margin: 0 0 .75rem;
      color: var(--biological);
      font-size: .78rem;
      font-weight: 700;
      letter-spacing: .14em;
      text-transform: uppercase;
    }
    h1, h2, h3 { line-height: 1.12; letter-spacing: -.025em; }
    h1 { max-width: 820px; margin: 0; font-size: clamp(2.35rem, 7vw, 5.1rem); font-weight: 750; }
    h2 { margin: 0; font-size: clamp(1.65rem, 4vw, 2.6rem); }
    h3 { margin: 0; font-size: 1.05rem; }
    .hero-copy { max-width: 760px; margin: 1.25rem 0 0; color: var(--muted); font-size: 1.08rem; }
    .hero-meta { display: flex; flex-wrap: wrap; gap: .7rem; margin-top: 1.6rem; }
    .status, .hash-chip {
      display: inline-flex;
      align-items: center;
      min-height: 2.15rem;
      padding: .35rem .75rem;
      border: 1px solid var(--border);
      border-radius: 999px;
      background: color-mix(in srgb, var(--surface) 86%, transparent);
      color: var(--muted);
      font: 650 .78rem/1 ui-monospace, SFMono-Regular, Consolas, monospace;
    }
    .status::before { content: ""; width: .5rem; height: .5rem; margin-right: .5rem; border-radius: 50%; background: var(--good); }
    .section-nav { display: flex; flex-wrap: wrap; gap: .45rem; margin-top: 1.5rem; }
    .section-nav a {
      padding: .45rem .7rem;
      border-radius: .45rem;
      color: var(--muted);
      text-decoration: none;
      font-size: .88rem;
    }
    .section-nav a:hover { background: var(--surface-2); color: var(--text); }
    main { padding-bottom: 5rem; }
    section { scroll-margin-top: 1.25rem; margin-top: 1.3rem; padding: clamp(1.15rem, 3.5vw, 2rem); border: 1px solid var(--border); border-radius: 1.2rem; background: color-mix(in srgb, var(--surface) 94%, transparent); box-shadow: var(--shadow); }
    .boundary { display: grid; grid-template-columns: minmax(0, 1fr) minmax(250px, .75fr); gap: 1.2rem; align-items: center; }
    .boundary p { margin: .6rem 0 0; color: var(--muted); }
    .boundary-list { display: grid; gap: .55rem; }
    .boundary-item { display: grid; grid-template-columns: 1.4rem 1fr; gap: .55rem; color: var(--muted); font-size: .9rem; }
    .boundary-item strong { color: var(--text); }
    .boundary-mark { display: grid; place-items: center; width: 1.35rem; height: 1.35rem; border-radius: 50%; background: color-mix(in srgb, var(--good) 18%, transparent); color: var(--good); font-size: .72rem; font-weight: 800; }
    .boundary-mark.no { background: color-mix(in srgb, var(--shock) 15%, transparent); color: var(--shock); }
    .section-head { display: flex; justify-content: space-between; gap: 1rem; align-items: end; margin-bottom: 1.15rem; }
    .section-head p { max-width: 650px; margin: .45rem 0 0; color: var(--muted); }
    .controls { display: flex; flex-wrap: wrap; gap: .42rem; }
    .control {
      appearance: none;
      min-height: 2.35rem;
      padding: .45rem .72rem;
      border: 1px solid var(--border);
      border-radius: .58rem;
      background: var(--surface-2);
      color: var(--muted);
      cursor: pointer;
    }
    .control:hover { color: var(--text); border-color: color-mix(in srgb, var(--fixed-class) 55%, var(--border)); }
    .control[aria-pressed="true"] { background: var(--text); color: var(--bg); border-color: var(--text); font-weight: 700; }
    .chart-wrap { min-width: 0; overflow: hidden; }
    #response-chart { display: block; width: 100%; height: auto; min-height: 270px; }
    .axis { stroke: var(--grid); stroke-width: 1; }
    .axis-label, .chart-label { fill: var(--muted); font: 12px ui-monospace, SFMono-Regular, Consolas, monospace; }
    .shock-band { fill: color-mix(in srgb, var(--shock) 12%, transparent); }
    .shock-line { stroke: var(--shock); stroke-width: 1.5; stroke-dasharray: 5 5; }
    .series-line { fill: none; stroke-width: 3.5; stroke-linecap: round; stroke-linejoin: round; }
    .series-point { stroke: var(--surface); stroke-width: 2.5; }
    .legend { display: flex; flex-wrap: wrap; gap: 1rem; margin: .65rem 0 0; color: var(--muted); font-size: .82rem; }
    .legend span { display: inline-flex; align-items: center; gap: .42rem; }
    .legend i { width: 1.15rem; height: .2rem; border-radius: 999px; background: var(--legend); }
    .generation-strip { display: grid; grid-template-columns: repeat(8, minmax(0, 1fr)); gap: .4rem; margin-top: 1rem; }
    .generation {
      min-width: 0;
      padding: .65rem .25rem;
      border: 1px solid var(--border);
      border-radius: .55rem;
      background: var(--surface-2);
      color: var(--muted);
      cursor: pointer;
    }
    .generation strong, .generation small { display: block; }
    .generation strong { color: var(--text); }
    .generation small { margin-top: .12rem; font-size: .68rem; text-transform: uppercase; letter-spacing: .08em; }
    .generation.shock { border-color: color-mix(in srgb, var(--shock) 60%, var(--border)); }
    .generation[aria-pressed="true"] { background: var(--text); color: var(--bg); border-color: var(--text); }
    .generation[aria-pressed="true"] strong { color: var(--bg); }
    .detail-grid { display: grid; grid-template-columns: 1.15fr repeat(3, 1fr); gap: .65rem; margin-top: .75rem; }
    .detail { min-width: 0; padding: .85rem; border-radius: .7rem; background: var(--surface-2); }
    .detail-label { color: var(--muted); font-size: .75rem; text-transform: uppercase; letter-spacing: .07em; }
    .detail-value { margin-top: .25rem; font: 720 1.15rem/1.25 ui-monospace, SFMono-Regular, Consolas, monospace; overflow-wrap: anywhere; }
    .detail-copy { margin: .25rem 0 0; color: var(--muted); font-size: .82rem; }
    .bars { display: grid; gap: 1rem; }
    .bar-row { display: grid; grid-template-columns: minmax(145px, .45fr) minmax(0, 1.55fr) minmax(82px, .32fr); gap: .75rem; align-items: center; }
    .bar-policy { min-width: 0; }
    .bar-policy strong { display: block; overflow-wrap: anywhere; }
    .bar-policy span { color: var(--muted); font-size: .76rem; }
    .bar-track { position: relative; display: flex; height: 2rem; border-radius: .42rem; background: var(--surface-3); overflow: hidden; }
    .bar-segment { min-width: 0; height: 100%; transition: width .25s ease; }
    .bar-segment.initial { background: var(--initial); }
    .bar-segment.repair { background: var(--repair); }
    .bar-value { text-align: right; font: 720 1rem/1 ui-monospace, SFMono-Regular, Consolas, monospace; }
    .bar-value small { display: block; margin-top: .25rem; color: var(--muted); font: 400 .7rem/1.2 ui-monospace, SFMono-Regular, Consolas, monospace; }
    .composition-legend { display: flex; flex-wrap: wrap; gap: 1rem; margin: 0 0 1rem; color: var(--muted); font-size: .82rem; }
    .composition-legend span { display: inline-flex; align-items: center; gap: .4rem; }
    .swatch { width: .78rem; height: .78rem; border-radius: .18rem; background: var(--swatch); }
    .finding { margin-top: 1.1rem; padding: .85rem 1rem; border-left: 3px solid var(--biological); background: var(--surface-2); color: var(--muted); }
    .finding strong { color: var(--text); }
    .metric-layout { display: grid; grid-template-columns: minmax(0, 1fr) minmax(230px, .42fr); gap: 1.2rem; }
    .metric-chart { display: grid; gap: .8rem; align-content: start; }
    .metric-row { display: grid; grid-template-columns: minmax(145px, .55fr) minmax(0, 1.5fr) minmax(90px, .35fr); gap: .65rem; align-items: center; }
    .metric-track { height: .72rem; border-radius: 999px; background: var(--surface-3); overflow: hidden; }
    .metric-fill { height: 100%; width: 0; border-radius: inherit; background: var(--policy-color); transition: width .25s ease; }
    .metric-value { text-align: right; font: 700 .84rem/1 ui-monospace, SFMono-Regular, Consolas, monospace; }
    .metric-note { padding: 1rem; border-radius: .8rem; background: var(--surface-2); color: var(--muted); }
    .metric-note strong { display: block; margin-bottom: .4rem; color: var(--text); font-size: 1.05rem; }
    .delivery-table { width: 100%; border-collapse: collapse; }
    .delivery-table th, .delivery-table td { padding: .72rem .6rem; text-align: left; border-bottom: 1px solid var(--border); }
    .delivery-table th { color: var(--muted); font-size: .76rem; letter-spacing: .06em; text-transform: uppercase; }
    .delivery-table td:not(:first-child), .delivery-table th:not(:first-child) { text-align: center; }
    .delivery-value { display: inline-flex; min-width: 3rem; justify-content: center; padding: .18rem .42rem; border-radius: .4rem; background: color-mix(in srgb, var(--good) 13%, transparent); color: var(--good); font: 700 .82rem/1.2 ui-monospace, SFMono-Regular, Consolas, monospace; }
    .delivery-value.shocked { background: color-mix(in srgb, var(--shock) 12%, transparent); color: var(--shock); }
    .provenance-grid { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: .7rem; }
    .provenance-item { min-width: 0; padding: .85rem; border-radius: .7rem; background: var(--surface-2); }
    .provenance-item dt { color: var(--muted); font-size: .75rem; text-transform: uppercase; letter-spacing: .06em; }
    .provenance-item dd { margin: .35rem 0 0; font: .8rem/1.45 ui-monospace, SFMono-Regular, Consolas, monospace; overflow-wrap: anywhere; }
    .evidence-links { display: flex; flex-wrap: wrap; gap: .75rem 1rem; margin-top: 1rem; }
    footer { padding: 1.4rem 0 3rem; color: var(--muted); font-size: .82rem; text-align: center; }
    .js-only { display: none; }
    @media (prefers-reduced-motion: reduce) {
      html { scroll-behavior: auto; }
      .bar-segment, .metric-fill { transition: none; }
    }
    @media (max-width: 760px) {
      .hero { padding-top: 2.3rem; }
      .boundary, .metric-layout { grid-template-columns: 1fr; }
      .section-head { align-items: start; flex-direction: column; }
      .generation-strip { grid-template-columns: repeat(4, minmax(0, 1fr)); }
      .detail-grid { grid-template-columns: repeat(2, minmax(0, 1fr)); }
      .detail-grid .detail:first-child { grid-column: 1 / -1; }
      .bar-row, .metric-row { grid-template-columns: minmax(105px, .52fr) minmax(0, 1.48fr); }
      .bar-value, .metric-value { grid-column: 2; }
      .provenance-grid { grid-template-columns: 1fr; }
    }
    @media (max-width: 470px) {
      .shell { width: min(100% - 1rem, 1120px); }
      section { border-radius: .85rem; padding: 1rem; }
      .generation-strip { grid-template-columns: repeat(4, minmax(0, 1fr)); gap: .3rem; }
      .detail-grid { grid-template-columns: 1fr; }
      .detail-grid .detail:first-child { grid-column: auto; }
      .bar-row, .metric-row { grid-template-columns: 1fr; gap: .35rem; }
      .bar-value, .metric-value { grid-column: 1; text-align: left; }
      .delivery-scroll { overflow-x: auto; }
      .delivery-table { min-width: 570px; }
    }
  </style>
</head>
<body>
  <a class="skip-link" href="#main">Skip to evidence</a>
  <header class="hero shell">
    <p class="eyebrow">Frozen raw-host evidence · Study v4</p>
    <h1>A failure happened. Only one policy changed its next plan.</h1>
    <p class="hero-copy">Explore how three runtime-selected policies behaved before, during and after the same imposed generation-2 failure. Every view below is derived from the published descriptive pilot, not a new simulation.</p>
    <div class="hero-meta">
      <span class="status">12 / 12 valid lifecycles</span>
      <span class="hash-chip" id="hero-analysis-hash">analysis · loading</span>
      <span class="hash-chip" id="hero-source-hash">source · loading</span>
    </div>
    <nav class="section-nav" aria-label="Explorer sections">
      <a href="#response">Causal response</a>
      <a href="#efficiency">Post-shock cost</a>
      <a href="#conditions">Transport conditions</a>
      <a href="#delivery">Delivery</a>
      <a href="#provenance">Provenance</a>
    </nav>
  </header>

  <main id="main" class="shell">
    <section class="boundary" aria-labelledby="boundary-title">
      <div>
        <p class="eyebrow">Evidence boundary</p>
        <h2 id="boundary-title">Promising for efficiency, not delivery.</h2>
        <p>The pilot was deliberately small and descriptive. Generation 2 is the common perturbation, never a policy result.</p>
      </div>
      <div class="boundary-list">
        <div class="boundary-item"><span class="boundary-mark">✓</span><span><strong>Observed:</strong> causal policy response and repair behavior.</span></div>
        <div class="boundary-item"><span class="boundary-mark no">×</span><span><strong>Not established:</strong> superiority, delivery advantage or field performance.</span></div>
      </div>
    </section>

    <section id="response" aria-labelledby="response-title">
      <div class="section-head">
        <div>
          <p class="eyebrow">01 · Causal response</p>
          <h2 id="response-title">Protection factor by generation</h2>
          <p>Select a traffic class, then step through the generation sequence. Generation 3 is the first plan made after the terminal failure.</p>
        </div>
        <div class="controls" id="class-controls" aria-label="Traffic class">
          <button class="control" type="button" data-class-index="0" aria-pressed="true">Critical</button>
          <button class="control" type="button" data-class-index="1" aria-pressed="false">Important</button>
          <button class="control" type="button" data-class-index="2" aria-pressed="false">Elastic</button>
        </div>
      </div>
      <div class="chart-wrap">
        <svg id="response-chart" viewBox="0 0 940 360" role="img" aria-labelledby="response-svg-title response-svg-desc">
          <title id="response-svg-title">Protection factor across eight generations</title>
          <desc id="response-svg-desc">Line chart comparing fixed-minimum, fixed-class-aware and biological-adaptive. Generation 2 is the imposed failure.</desc>
        </svg>
      </div>
      <div class="legend" aria-label="Policy legend">
        <span><i style="--legend: var(--fixed-minimum)"></i>fixed-minimum</span>
        <span><i style="--legend: var(--fixed-class)"></i>fixed-class-aware</span>
        <span><i style="--legend: var(--biological)"></i>biological-adaptive</span>
      </div>
      <div class="generation-strip" id="generation-strip" aria-label="Generation selection"></div>
      <div class="detail-grid" aria-live="polite">
        <div class="detail">
          <div class="detail-label" id="generation-context-label">Selected generation</div>
          <div class="detail-value" id="generation-context-value">Generation 3</div>
          <p class="detail-copy" id="generation-context-copy"></p>
        </div>
        <div class="detail">
          <div class="detail-label">fixed-minimum</div>
          <div class="detail-value" id="factor-minimum">—</div>
          <p class="detail-copy">selected-class factor</p>
        </div>
        <div class="detail">
          <div class="detail-label">fixed-class-aware</div>
          <div class="detail-value" id="factor-class-aware">—</div>
          <p class="detail-copy">selected-class factor</p>
        </div>
        <div class="detail">
          <div class="detail-label">biological state at plan</div>
          <div class="detail-value" id="biological-state">—</div>
          <p class="detail-copy" id="biological-factor">selected-class factor</p>
        </div>
      </div>
    </section>

    <section id="efficiency" aria-labelledby="efficiency-title">
      <div class="section-head">
        <div>
          <p class="eyebrow">02 · Post-shock cost</p>
          <h2 id="efficiency-title">Initial protection + emitted repair</h2>
          <p>Five prescheduled generations, 200 source symbols per lifecycle. Requested repair is shown as context but is not added to the wire total.</p>
        </div>
        <div class="controls" id="block-controls" aria-label="Block view">
          <button class="control" type="button" data-block="mean" aria-pressed="true">Mean</button>
          <button class="control" type="button" data-block="0" aria-pressed="false">Block 1</button>
          <button class="control" type="button" data-block="1" aria-pressed="false">Block 2</button>
        </div>
      </div>
      <div class="composition-legend">
        <span><i class="swatch" style="--swatch: var(--initial)"></i>initial symbols</span>
        <span><i class="swatch" style="--swatch: var(--repair)"></i>repair symbols emitted</span>
      </div>
      <div class="bars" id="post-shock-bars"></div>
      <div class="finding" id="post-shock-finding" aria-live="polite"></div>
    </section>

    <section id="conditions" aria-labelledby="conditions-title">
      <div class="section-head">
        <div>
          <p class="eyebrow">03 · Whole-run transport</p>
          <h2 id="conditions-title">Same binary, two conditions</h2>
          <p>Means across the two randomized blocks. Change the outcome without changing the treatment source.</p>
        </div>
        <div>
          <div class="controls" id="condition-controls" aria-label="Transport condition">
            <button class="control" type="button" data-condition="timed-replay-v2" aria-pressed="false">Timed replay</button>
            <button class="control" type="button" data-condition="regime-change-v1" aria-pressed="true">Regime change</button>
          </div>
          <div class="controls" id="metric-controls" aria-label="Transport metric" style="margin-top: .45rem">
            <button class="control" type="button" data-metric="wire_symbols" aria-pressed="true">Wire symbols</button>
            <button class="control" type="button" data-metric="repair_emitted" aria-pressed="false">Repair emitted</button>
            <button class="control" type="button" data-metric="terminal_feedback_rtt_mean_us" aria-pressed="false">Feedback RTT</button>
            <button class="control" type="button" data-metric="terminal_feedback_retry_rounds" aria-pressed="false">Retry rounds</button>
          </div>
        </div>
      </div>
      <div class="metric-layout">
        <div class="metric-chart" id="condition-bars"></div>
        <div class="metric-note" id="condition-note" aria-live="polite"></div>
      </div>
    </section>

    <section id="delivery" aria-labelledby="delivery-title">
      <div class="section-head">
        <div>
          <p class="eyebrow">04 · Delivery guardrail</p>
          <h2 id="delivery-title">Delivery saturated after the shock</h2>
          <p>Both conditions are shown in frozen block order. In regime-change-v1 the shared generation-2 failure explains 7/8; generations 3–7 were 5/5 for every policy.</p>
        </div>
      </div>
      <div class="delivery-scroll">
        <table class="delivery-table">
          <thead><tr><th>Condition</th><th>Policy</th><th>Block 1</th><th>Block 2</th><th>Interpretation</th></tr></thead>
          <tbody id="delivery-body"></tbody>
        </table>
      </div>
      <div class="finding"><strong>Do not rank generation 2.</strong> It was the deterministic policy-neutral perturbation. The informative response begins at generation 3.</div>
    </section>

    <section id="provenance" aria-labelledby="provenance-title">
      <div class="section-head">
        <div>
          <p class="eyebrow">05 · Provenance</p>
          <h2 id="provenance-title">Trace every view to frozen evidence</h2>
          <p>The explorer is a generated reporting surface. It performs no network request and contains the evidence input inline.</p>
        </div>
      </div>
      <dl class="provenance-grid" id="provenance-grid"></dl>
      <div class="evidence-links">
        <a href="raw-host-policy-pilot-v1-final-results.md">Read the final descriptive result</a>
        <a href="../benchmarks/raw_host_policy_pilot_v1_explorer_input.json">Inspect the explorer input</a>
        <a href="https://github.com/Daniele-Cangi/Aurora/releases/tag/raw-host-policy-pilot-v1-study-v4">Open the immutable GitHub Release</a>
      </div>
    </section>
  </main>

  <footer class="shell">Aurora raw-host policy pilot v1 · descriptive evidence only · no live GCP connection</footer>
  <noscript><p class="shell">This evidence explorer requires JavaScript to render the frozen inline dataset.</p></noscript>
  <script id="evidence-payload" type="application/json">__EVIDENCE_PAYLOAD__</script>
  <script>
    (() => {
      "use strict";
      const payload = JSON.parse(document.getElementById("evidence-payload").textContent);
      const data = payload.evidence;
      const policies = ["fixed-minimum", "fixed-class-aware", "biological-adaptive"];
      const policyLabels = {
        "fixed-minimum": "fixed-minimum",
        "fixed-class-aware": "fixed-class-aware",
        "biological-adaptive": "biological-adaptive"
      };
      const policyColors = {
        "fixed-minimum": "var(--fixed-minimum)",
        "fixed-class-aware": "var(--fixed-class)",
        "biological-adaptive": "var(--biological)"
      };
      const classNames = ["critical", "important", "elastic"];
      const metricMeta = {
        wire_symbols: {label: "wire symbols", unit: "symbols", format: value => formatNumber(value)},
        repair_emitted: {label: "repair emitted", unit: "symbols", format: value => formatNumber(value)},
        terminal_feedback_rtt_mean_us: {label: "terminal feedback RTT", unit: "ms", format: value => formatNumber(value / 1000, 3)},
        terminal_feedback_retry_rounds: {label: "terminal retry rounds", unit: "rounds", format: value => formatNumber(value, 1)}
      };
      let selectedClass = 0;
      let selectedGeneration = 3;
      let selectedBlock = "mean";
      let selectedCondition = "regime-change-v1";
      let selectedMetric = "wire_symbols";

      function formatNumber(value, maxDigits = 2) {
        return new Intl.NumberFormat("en-US", {maximumFractionDigits: maxDigits}).format(value);
      }

      function shortHash(value) {
        return value.slice(0, 10) + "…" + value.slice(-6);
      }

      function mean(values) {
        return values.reduce((sum, value) => sum + value, 0) / values.length;
      }

      function phase(generation) {
        if (data.windows.pre_shock_generation_indices.includes(generation)) return "pre-shock";
        if (generation === data.windows.imposed_shock_generation_index) return "imposed shock";
        return "post-shock";
      }

      function updatePressed(containerId, attribute, value) {
        document.querySelectorAll(`#${containerId} [${attribute}]`).forEach(button => {
          button.setAttribute("aria-pressed", String(button.getAttribute(attribute) === String(value)));
        });
      }

      function renderResponseChart() {
        const svg = document.getElementById("response-chart");
        const width = Math.max(320, Math.round(svg.getBoundingClientRect().width));
        svg.setAttribute("viewBox", `0 0 ${width} 360`);
        const compact = width < 520;
        const x0 = compact ? 54 : 78;
        const x1 = width - (compact ? 18 : 40);
        const y0 = 294;
        const y1 = 36;
        const x = generation => x0 + (x1 - x0) * generation / 7;
        const y = factor => y0 - (factor - 0.75) / (4.25 - 0.75) * (y0 - y1);
        const shockWidth = Math.min(68, (x1 - x0) / 7 * .65);
        const pieces = [
          '<title id="response-svg-title">Protection factor across eight generations</title>',
          `<desc id="response-svg-desc">${classNames[selectedClass]} protection factors for all three policies. Generation 2 is the imposed failure.</desc>`,
          `<rect class="shock-band" x="${x(2) - shockWidth / 2}" y="${y1}" width="${shockWidth}" height="${y0 - y1}"></rect>`
        ];
        [1, 2, 3, 4].forEach(tick => {
          pieces.push(`<line class="axis" x1="${x0}" y1="${y(tick)}" x2="${x1}" y2="${y(tick)}"></line>`);
          pieces.push(`<text class="axis-label" x="${x0 - 16}" y="${y(tick) + 4}" text-anchor="end">${tick.toFixed(1)}</text>`);
        });
        for (let generation = 0; generation < 8; generation += 1) {
          pieces.push(`<text class="axis-label" x="${x(generation)}" y="322" text-anchor="middle">g${generation}</text>`);
        }
        pieces.push(`<line class="shock-line" x1="${x(2)}" y1="${y1}" x2="${x(2)}" y2="${y0}"></line>`);
        pieces.push(`<text class="chart-label" x="${x(2)}" y="24" text-anchor="middle">${compact ? "shock" : "imposed failure"}</text>`);
        pieces.push('<text class="chart-label" x="18" y="166" transform="rotate(-90 18 166)" text-anchor="middle">protection factor</text>');
        policies.forEach(policy => {
          const values = data.policies[policy].protection_factors.map(row => row[selectedClass]);
          const points = values.map((value, generation) => `${x(generation)},${y(value)}`).join(" ");
          pieces.push(`<polyline class="series-line" points="${points}" style="stroke:${policyColors[policy]}"></polyline>`);
          values.forEach((value, generation) => {
            const radius = generation === selectedGeneration ? 7 : 4.5;
            pieces.push(`<circle class="series-point" cx="${x(generation)}" cy="${y(value)}" r="${radius}" style="fill:${policyColors[policy]}"><title>${policy}, generation ${generation}: ${formatNumber(value, 3)}</title></circle>`);
          });
        });
        svg.innerHTML = pieces.join("");
      }

      function renderGenerationStrip() {
        const strip = document.getElementById("generation-strip");
        strip.innerHTML = "";
        for (let generation = 0; generation < 8; generation += 1) {
          const button = document.createElement("button");
          button.type = "button";
          button.className = "generation" + (generation === 2 ? " shock" : "");
          button.dataset.generation = String(generation);
          button.setAttribute("aria-pressed", String(generation === selectedGeneration));
          button.innerHTML = `<strong>g${generation}</strong><small>${phase(generation)}</small>`;
          button.addEventListener("click", () => {
            selectedGeneration = generation;
            renderGenerationStrip();
            renderGenerationDetail();
            renderResponseChart();
          });
          strip.appendChild(button);
        }
      }

      function renderGenerationDetail() {
        const generation = selectedGeneration;
        const stateEntry = data.policies["biological-adaptive"].adaptive_state[generation];
        const state = stateEntry.at_plan;
        const biologicalFactor = data.policies["biological-adaptive"].protection_factors[generation][selectedClass];
        document.getElementById("generation-context-value").textContent = `Generation ${generation} · ${phase(generation)}`;
        let context = "Plan produced before any imposed perturbation.";
        if (generation === 2) {
          const after = stateEntry.after_terminal;
          context = `Terminal failure was imposed after this plan. State became ${after.success} success / ${after.failure} failure / ${after.panic} panic.`;
        } else if (generation === 3) {
          context = "First plan after failure: the updated biological state is already reflected in protection.";
        } else if (generation > 3) {
          context = "Post-shock planning after authenticated terminal feedback.";
        }
        document.getElementById("generation-context-copy").textContent = context;
        document.getElementById("factor-minimum").textContent = formatNumber(data.policies["fixed-minimum"].protection_factors[generation][selectedClass], 3);
        document.getElementById("factor-class-aware").textContent = formatNumber(data.policies["fixed-class-aware"].protection_factors[generation][selectedClass], 3);
        document.getElementById("biological-state").textContent = `${state.success} / ${state.failure} / ${state.panic}`;
        document.getElementById("biological-factor").textContent = `${classNames[selectedClass]} factor ${formatNumber(biologicalFactor, 3)} · success / failure / panic`;
      }

      function valueForBlock(values) {
        return selectedBlock === "mean" ? mean(values) : values[Number(selectedBlock)];
      }

      function renderPostShock() {
        const container = document.getElementById("post-shock-bars");
        const rows = policies.map(policy => {
          const evidence = data.post_shock.policies[policy];
          return {
            policy,
            initial: valueForBlock(evidence.initial_symbols),
            repair: valueForBlock(evidence.repair_emitted),
            requested: valueForBlock(evidence.repair_requested),
            wire: valueForBlock(evidence.wire_symbols),
            delivered: valueForBlock(evidence.critical_before_deadline)
          };
        });
        const ceiling = Math.max(...rows.map(row => row.wire));
        container.innerHTML = rows.map(row => {
          const initialWidth = row.initial / ceiling * 100;
          const repairWidth = row.repair / ceiling * 100;
          return `<div class="bar-row">
            <div class="bar-policy"><strong>${policyLabels[row.policy]}</strong><span>${formatNumber(row.delivered)}/5 critical · ${formatNumber(row.requested)} repair requested</span></div>
            <div class="bar-track" role="img" aria-label="${row.policy}: ${formatNumber(row.initial)} initial plus ${formatNumber(row.repair)} repair equals ${formatNumber(row.wire)} wire symbols">
              <span class="bar-segment initial" style="width:${initialWidth}%"></span><span class="bar-segment repair" style="width:${repairWidth}%"></span>
            </div>
            <div class="bar-value">${formatNumber(row.wire)}<small>wire symbols</small></div>
          </div>`;
        }).join("");
        const viewLabel = selectedBlock === "mean" ? "Across both blocks" : `In block ${Number(selectedBlock) + 1}`;
        const comparison = payload.copy.post_shock_comparisons[selectedBlock];
        document.getElementById("post-shock-finding").innerHTML = `<strong>${viewLabel}, ${comparison} versus fixed-class-aware.</strong> Biological paid more initial protection and emitted fewer repair symbols; delivery remained 5/5 for both.`;
      }

      function renderCondition() {
        const rows = data.condition_means_across_two_blocks.filter(cell => cell.condition === selectedCondition);
        const maxValue = Math.max(...rows.map(row => row[selectedMetric]));
        const meta = metricMeta[selectedMetric];
        document.getElementById("condition-bars").innerHTML = rows.map(row => `<div class="metric-row">
          <strong>${policyLabels[row.policy]}</strong>
          <div class="metric-track" role="img" aria-label="${row.policy}: ${meta.format(row[selectedMetric])} ${meta.unit}"><div class="metric-fill" style="--policy-color:${policyColors[row.policy]};width:${row[selectedMetric] / maxValue * 100}%"></div></div>
          <div class="metric-value">${meta.format(row[selectedMetric])} ${meta.unit}</div>
        </div>`).join("");
        const fixed = rows.find(row => row.policy === "fixed-class-aware");
        const biological = rows.find(row => row.policy === "biological-adaptive");
        const delta = biological[selectedMetric] - fixed[selectedMetric];
        const percent = fixed[selectedMetric] === 0 ? 0 : delta / fixed[selectedMetric] * 100;
        const direction = delta === 0 ? "equal to" : delta < 0 ? "lower than" : "higher than";
        let note = `Biological is ${formatNumber(Math.abs(percent), 2)}% ${direction} fixed-class-aware for this descriptive mean.`;
        if (selectedMetric === "terminal_feedback_rtt_mean_us") {
          note += " This is sender-steady application RTT, not one-way or network-only latency.";
        }
        document.getElementById("condition-note").innerHTML = `<strong>${selectedCondition} · ${meta.label}</strong><span>${note}</span>`;
      }

      function renderDelivery() {
        const body = document.getElementById("delivery-body");
        const rows = [];
        ["timed-replay-v2", "regime-change-v1"].forEach(condition => {
          policies.forEach(policy => {
            const values = data.delivery[condition][policy];
            const shocked = condition === "regime-change-v1";
            rows.push(`<tr>
              <td>${condition}</td><td>${policyLabels[policy]}</td>
              <td><span class="delivery-value${shocked ? " shocked" : ""}">${values[0]}</span></td>
              <td><span class="delivery-value${shocked ? " shocked" : ""}">${values[1]}</span></td>
              <td>${shocked ? "common g2 failure" : "all delivered"}</td>
            </tr>`);
          });
        });
        body.innerHTML = rows.join("");
      }

      function renderProvenance() {
        const p = data.provenance;
        const d = data.deadline_semantics;
        const values = [
          ["Evidence archive SHA-256", p.evidence_archive_sha256],
          ["Analysis JSON SHA-256", p.analysis_json_sha256],
          ["Runtime binary SHA-256", p.runtime_binary_sha256],
          ["Experimental source", p.experimental_source],
          ["Explorer input SHA-256", payload.input_sha256],
          ["Deadline clock", `${d.clock} · ${d.violations} violations`]
        ];
        document.getElementById("provenance-grid").innerHTML = values.map(([label, value]) => `<div class="provenance-item"><dt>${label}</dt><dd>${value}</dd></div>`).join("");
        document.getElementById("hero-analysis-hash").textContent = `analysis · ${shortHash(p.analysis_json_sha256)}`;
        document.getElementById("hero-source-hash").textContent = `source · ${shortHash(p.experimental_source)}`;
      }

      document.querySelectorAll("#class-controls [data-class-index]").forEach(button => {
        button.addEventListener("click", () => {
          selectedClass = Number(button.dataset.classIndex);
          updatePressed("class-controls", "data-class-index", selectedClass);
          renderResponseChart();
          renderGenerationDetail();
        });
      });
      document.querySelectorAll("#block-controls [data-block]").forEach(button => {
        button.addEventListener("click", () => {
          selectedBlock = button.dataset.block;
          updatePressed("block-controls", "data-block", selectedBlock);
          renderPostShock();
        });
      });
      document.querySelectorAll("#condition-controls [data-condition]").forEach(button => {
        button.addEventListener("click", () => {
          selectedCondition = button.dataset.condition;
          updatePressed("condition-controls", "data-condition", selectedCondition);
          renderCondition();
        });
      });
      document.querySelectorAll("#metric-controls [data-metric]").forEach(button => {
        button.addEventListener("click", () => {
          selectedMetric = button.dataset.metric;
          updatePressed("metric-controls", "data-metric", selectedMetric);
          renderCondition();
        });
      });

      renderResponseChart();
      renderGenerationStrip();
      renderGenerationDetail();
      renderPostShock();
      renderCondition();
      renderDelivery();
      renderProvenance();
      let lastChartWidth = 0;
      new ResizeObserver(entries => {
        const width = Math.round(entries[0].contentRect.width);
        if (Math.abs(width - lastChartWidth) > 1) {
          lastChartWidth = width;
          renderResponseChart();
        }
      }).observe(document.querySelector("#response .chart-wrap"));
    })();
  </script>
</body>
</html>
'''


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--input", type=Path, default=DEFAULT_INPUT)
    result.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    return result


def main(argv: Iterable[str] | None = None) -> int:
    args = parser().parse_args(argv)
    value = load_json(args.input)
    validate_input(value)
    write_lf(args.output, render_explorer(value, sha256_file(args.input)))
    print(json.dumps({
        "input": str(args.input),
        "input_sha256": sha256_file(args.input),
        "output": str(args.output),
        "output_sha256": sha256_file(args.output),
    }, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ExplorerError as error:
        print(f"raw-host evidence explorer: {error}")
        raise SystemExit(1)
