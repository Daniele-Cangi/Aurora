#!/usr/bin/env python3
"""Render deterministic SVG evidence for the raw-host policy studies.

Repository visuals are data-neutral or use the published descriptive pilot
input.  Passing a frozen confirmatory analysis additionally renders the four
figures registered before dispatch and a checksum manifest.
"""

from __future__ import annotations

import argparse
from collections import defaultdict
from dataclasses import dataclass
import hashlib
from html import escape
import json
import math
from pathlib import Path
import statistics
from typing import Iterable


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_PILOT_INPUT = (
    ROOT / "benchmarks" / "raw_host_policy_pilot_v1_visualization_input.json"
)
DEFAULT_STUDY_SPEC = (
    ROOT / "benchmarks" / "gcp_raw_post_shock_efficiency_study_v1.json"
)
DEFAULT_VISUAL_SPEC = (
    ROOT / "benchmarks" /
    "raw_host_post_shock_efficiency_visualization_v1.json"
)

PILOT_SCHEMA = "aurora-raw-host-policy-pilot-visualization-input-v1"
STUDY_SCHEMA = "aurora-gcp-raw-post-shock-efficiency-study-plan-v1"
VISUAL_SCHEMA = "aurora-raw-host-post-shock-efficiency-visualization-v1"
ANALYSIS_SCHEMA = "aurora-raw-post-shock-efficiency-analysis-v1"
STUDY_ID = "raw-host-post-shock-efficiency-study-v1"
POLICIES = ("fixed-class-aware", "biological-adaptive")
POLICY_COLORS = {
    "fixed-minimum": "#667085",
    "fixed-class-aware": "#2563EB",
    "biological-adaptive": "#EA580C",
}
CLASS_NAMES = ("Critical", "Important", "Elastic")


class VisualError(RuntimeError):
    pass


def load_json(path: Path) -> dict:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise VisualError(f"invalid JSON input: {path}") from error
    if not isinstance(value, dict):
        raise VisualError(f"JSON input is not an object: {path}")
    return value


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(block)
    except OSError as error:
        raise VisualError(f"cannot hash file: {path}") from error
    return digest.hexdigest()


def write_lf(path: Path, value: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    try:
        with path.open("w", encoding="utf-8", newline="\n") as stream:
            stream.write(value.rstrip() + "\n")
    except OSError as error:
        raise VisualError(f"cannot write visual: {path}") from error


def number(value: object, label: str) -> float:
    if not isinstance(value, (int, float)) or isinstance(value, bool) or \
            not math.isfinite(float(value)):
        raise VisualError(f"{label} is not a finite number")
    return float(value)


def integer(value: object, label: str) -> int:
    result = number(value, label)
    if result != int(result):
        raise VisualError(f"{label} is not an integer")
    return int(result)


def fmt(value: float) -> str:
    if math.isclose(value, round(value), abs_tol=1e-9):
        return str(int(round(value)))
    return f"{value:.2f}".rstrip("0").rstrip(".")


def scale(
    value: float,
    domain_min: float,
    domain_max: float,
    range_min: float,
    range_max: float,
) -> float:
    if math.isclose(domain_min, domain_max):
        return (range_min + range_max) / 2.0
    fraction = (value - domain_min) / (domain_max - domain_min)
    return range_min + fraction * (range_max - range_min)


def nice_domain(values: Iterable[float], *, include_zero: bool = False) -> tuple:
    observed = list(values)
    if include_zero:
        observed.append(0.0)
    if not observed:
        return 0.0, 1.0
    low = min(observed)
    high = max(observed)
    span = high - low
    padding = max(span * 0.12, max(abs(low), abs(high), 1.0) * 0.06)
    return low - padding, high + padding


@dataclass
class Svg:
    width: int
    height: int
    items: list[str]

    @staticmethod
    def create(width: int, height: int) -> "Svg":
        return Svg(width, height, [])

    @staticmethod
    def attrs(**values: object) -> str:
        parts = []
        for key, value in values.items():
            if value is None:
                continue
            name = key.rstrip("_").replace("_", "-")
            parts.append(f'{name}="{escape(str(value), quote=True)}"')
        return " ".join(parts)

    def raw(self, value: str) -> None:
        self.items.append(value)

    def text(
        self,
        x: float,
        y: float,
        value: object,
        *,
        class_: str = "label",
        anchor: str = "start",
        **attrs: object,
    ) -> None:
        attributes = self.attrs(
            x=round(x, 2), y=round(y, 2), class_=class_,
            text_anchor=anchor, **attrs,
        )
        self.items.append(f"<text {attributes}>{escape(str(value))}</text>")

    def line(
        self,
        x1: float,
        y1: float,
        x2: float,
        y2: float,
        *,
        class_: str = "grid",
        **attrs: object,
    ) -> None:
        attributes = self.attrs(
            x1=round(x1, 2), y1=round(y1, 2),
            x2=round(x2, 2), y2=round(y2, 2),
            class_=class_, **attrs,
        )
        self.items.append(f"<line {attributes}/>")

    def rect(
        self,
        x: float,
        y: float,
        width: float,
        height: float,
        *,
        class_: str | None = None,
        **attrs: object,
    ) -> None:
        attributes = self.attrs(
            x=round(x, 2), y=round(y, 2),
            width=round(width, 2), height=round(height, 2),
            class_=class_, **attrs,
        )
        self.items.append(f"<rect {attributes}/>")

    def circle(
        self,
        cx: float,
        cy: float,
        radius: float,
        *,
        class_: str | None = None,
        **attrs: object,
    ) -> None:
        attributes = self.attrs(
            cx=round(cx, 2), cy=round(cy, 2), r=round(radius, 2),
            class_=class_, **attrs,
        )
        self.items.append(f"<circle {attributes}/>")

    def polyline(
        self,
        points: Iterable[tuple[float, float]],
        *,
        class_: str | None = None,
        **attrs: object,
    ) -> None:
        joined = " ".join(f"{x:.2f},{y:.2f}" for x, y in points)
        attributes = self.attrs(points=joined, class_=class_, **attrs)
        self.items.append(f"<polyline {attributes}/>")

    def finish(self, title: str, description: str) -> str:
        style = """
    :root { color-scheme: light dark; }
    .surface { fill: #FFFFFF; }
    .title { fill: #101828; font: 700 26px system-ui, sans-serif; }
    .subtitle { fill: #475467; font: 400 14px system-ui, sans-serif; }
    .heading { fill: #101828; font: 700 16px system-ui, sans-serif; }
    .label { fill: #344054; font: 500 13px system-ui, sans-serif; }
    .small { fill: #475467; font: 400 12px system-ui, sans-serif; }
    .tiny { fill: #667085; font: 400 11px system-ui, sans-serif; }
    .value { fill: #101828; font: 700 14px system-ui, sans-serif; }
    .panel { fill: #F8FAFC; stroke: #D0D5DD; stroke-width: 1; }
    .box { fill: #FFFFFF; stroke: #98A2B3; stroke-width: 1.2; }
    .fixed-box { fill: #EFF6FF; stroke: #2563EB; stroke-width: 1.5; }
    .adaptive-box { fill: #FFF7ED; stroke: #EA580C; stroke-width: 1.5; }
    .shock-box { fill: #FEF3F2; stroke: #D92D20; stroke-width: 1.5; }
    .post-box { fill: #ECFDF3; stroke: #12B76A; stroke-width: 1.2; }
    .grid { stroke: #E4E7EC; stroke-width: 1; }
    .axis { stroke: #667085; stroke-width: 1.2; }
    .flow { stroke: #475467; stroke-width: 1.6; fill: none; }
    .shock-line { stroke: #D92D20; stroke-width: 1.4; stroke-dasharray: 5 4; }
    .reference { stroke: #98A2B3; stroke-width: 1.2; stroke-dasharray: 5 4; }
    .mre { stroke: #7F56D9; stroke-width: 1.4; stroke-dasharray: 4 4; }
    .fixed-line { stroke: #2563EB; stroke-width: 2.5; fill: none; }
    .adaptive-line { stroke: #EA580C; stroke-width: 2.5; fill: none; }
    .minimum-line { stroke: #667085; stroke-width: 2.2; fill: none; }
    .initial { fill: #2563EB; }
    .repair { fill: #F79009; }
    .success { fill: #12B76A; }
    .failure { fill: #D92D20; }
    .neutral { fill: #667085; }
    .mean { stroke: #101828; stroke-width: 3; fill: #101828; }
    .range { fill: #F2F4F7; stroke: none; opacity: .8; }
    @media (prefers-color-scheme: dark) {
      .surface { fill: #0B1220; }
      .title, .heading, .value { fill: #F9FAFB; }
      .subtitle, .label, .small, .tiny { fill: #D0D5DD; }
      .panel { fill: #111827; stroke: #475467; }
      .box { fill: #111827; stroke: #667085; }
      .fixed-box { fill: #102A56; }
      .adaptive-box { fill: #4A2410; }
      .shock-box { fill: #451A1A; }
      .post-box { fill: #123524; }
      .grid { stroke: #344054; }
      .axis, .flow, .reference { stroke: #98A2B3; }
      .range { fill: #344054; }
      .mean { stroke: #F9FAFB; fill: #F9FAFB; }
    }
""".strip()
        body = "\n  ".join(self.items)
        return (
            f'<svg xmlns="http://www.w3.org/2000/svg" '
            f'viewBox="0 0 {self.width} {self.height}" role="img" '
            f'aria-labelledby="visual-title visual-desc">\n'
            f"  <title id=\"visual-title\">{escape(title)}</title>\n"
            f"  <desc id=\"visual-desc\">{escape(description)}</desc>\n"
            f"  <style>{style}</style>\n"
            f'  <rect class="surface" width="{self.width}" '
            f'height="{self.height}"/>\n  {body}\n</svg>'
        )


def validate_pilot(value: dict) -> None:
    if value.get("schema") != PILOT_SCHEMA or \
            value.get("pilot_id") != "raw-host-policy-pilot-v1-study-v4":
        raise VisualError("unexpected pilot visualization input")
    if value.get("condition") != "regime-change-v1" or \
            value.get("windows", {}).get("imposed_shock_generation_index") != 2:
        raise VisualError("pilot shock window changed")
    factors = value.get("protection_factors", {})
    expected_policies = {
        "fixed-minimum", "fixed-class-aware", "biological-adaptive"
    }
    if set(factors) != expected_policies:
        raise VisualError("pilot visualization needs all three policies")
    for policy, generations in factors.items():
        if not isinstance(generations, list) or len(generations) != 8:
            raise VisualError(f"{policy} does not have eight protection plans")
        for index, plan in enumerate(generations):
            if not isinstance(plan, list) or len(plan) != 3:
                raise VisualError(f"{policy} generation {index} plan is invalid")
            for class_index, factor in enumerate(plan):
                number(factor, f"{policy} generation {index} factor {class_index}")
    blocks = value.get("post_shock_blocks")
    if not isinstance(blocks, list) or len(blocks) != 2:
        raise VisualError("pilot visualization needs the two published blocks")
    for expected_block, block in enumerate(blocks, start=1):
        if block.get("block") != expected_block or \
                set(block) != {"block", *POLICIES}:
            raise VisualError("pilot block identity changed")
        for policy in POLICIES:
            outcome = block[policy]
            initial = integer(outcome.get("initial_symbols"), "initial symbols")
            repair = integer(
                outcome.get("repair_symbols_emitted"), "repair symbols"
            )
            total = integer(
                outcome.get("wire_symbol_datagrams"), "wire symbols"
            )
            if initial + repair != total or \
                    outcome.get("critical_before_deadline") != 5 or \
                    outcome.get("scheduled_generations") != 5:
                raise VisualError("pilot post-shock outcome is inconsistent")
    claims = value.get("claim_boundary", {})
    if claims.get("descriptive") is not True or \
            claims.get("confirmatory") is not False or \
            claims.get("policy_superiority") is not False:
        raise VisualError("pilot claim boundary changed")


def validate_study(study: dict, visual: dict) -> None:
    if study.get("schema") != STUDY_SCHEMA or \
            study.get("study_id") != STUDY_ID or study.get("blocks") != 23 or \
            study.get("treatments") != list(POLICIES):
        raise VisualError("unexpected post-shock study specification")
    if study.get("condition", {}).get("outage_generation_index") != 2 or \
            study.get("execution", {}).get("lifecycle_count") != 46:
        raise VisualError("study lifecycle or shock design changed")
    if visual.get("schema") != VISUAL_SCHEMA or \
            visual.get("study_id") != STUDY_ID or \
            visual.get("analysis_input_schema") != ANALYSIS_SCHEMA or \
            visual.get("registered_before_dispatch") is not True:
        raise VisualError("unexpected visualization specification")
    figures = visual.get("figures")
    expected_ids = {
        "primary-paired-contrast",
        "generation-protection-response",
        "post-shock-wire-composition",
        "delivery-guardrail",
    }
    if not isinstance(figures, list) or \
            {figure.get("id") for figure in figures} != expected_ids:
        raise VisualError("registered figure set changed")
    constraints = visual.get("presentation_constraints", {})
    required_true = (
        "show_all_blocks",
        "show_shock_as_common_imposed_perturbation",
        "include_provenance_manifest",
    )
    required_false = (
        "rank_generation_2_by_policy",
        "truncate_primary_axis",
        "hide_delivery_misses",
        "replace_missing_or_failed_generations",
        "outcome_dependent_palette_or_order",
    )
    if any(constraints.get(key) is not True for key in required_true) or \
            any(constraints.get(key) is not False for key in required_false):
        raise VisualError("visual presentation constraints changed")


def arrow(svg: Svg, x1: float, y1: float, x2: float, y2: float) -> None:
    svg.line(x1, y1, x2, y2, class_="flow")
    angle = math.atan2(y2 - y1, x2 - x1)
    length = 9
    spread = 0.55
    points = [(x2, y2)]
    for offset in (-spread, spread):
        points.append((
            x2 - length * math.cos(angle + offset),
            y2 - length * math.sin(angle + offset),
        ))
    svg.polyline([points[1], points[0], points[2]], class_="flow")


def box(
    svg: Svg,
    x: float,
    y: float,
    width: float,
    height: float,
    title: str,
    detail: str,
    *,
    class_: str = "box",
) -> None:
    svg.rect(x, y, width, height, class_=class_, rx=10)
    svg.text(x + width / 2, y + 29, title, class_="heading", anchor="middle")
    svg.text(x + width / 2, y + 52, detail, class_="small", anchor="middle")


def render_study_design(study: dict) -> str:
    svg = Svg.create(1200, 680)
    svg.text(55, 48, "Raw-host post-shock efficiency study", class_="title")
    svg.text(
        55, 76,
        "One frozen binary, runtime-selected policies, one common shock and paired evidence",
        class_="subtitle",
    )

    box(svg, 55, 115, 190, 82, "Frozen binary", "identical SHA-256")
    box(svg, 300, 115, 200, 82, "Runtime selection", "sender --policy")
    arrow(svg, 245, 156, 300, 156)
    box(
        svg, 555, 96, 215, 72,
        "fixed-class-aware", "unchanged plan", class_="fixed-box",
    )
    box(
        svg, 555, 184, 215, 72,
        "biological-adaptive", "feedback-driven plan", class_="adaptive-box",
    )
    arrow(svg, 500, 145, 555, 132)
    arrow(svg, 500, 167, 555, 220)
    box(svg, 835, 115, 150, 82, "Sender", "descriptor + symbols")
    box(svg, 1035, 115, 120, 82, "Receiver", "local deadlines")
    arrow(svg, 770, 132, 835, 151)
    arrow(svg, 770, 220, 835, 174)
    arrow(svg, 985, 145, 1035, 145)
    svg.text(1010, 132, "authenticated UDP", class_="tiny", anchor="middle")
    svg.line(1095, 200, 1095, 300, class_="flow")
    svg.line(1095, 300, 400, 300, class_="flow")
    arrow(svg, 400, 300, 400, 197)
    svg.text(
        748, 322,
        "same authenticated terminal feedback applied before the next plan",
        class_="small",
        anchor="middle",
    )

    svg.text(55, 350, "Eight-generation causal timeline", class_="heading")
    start_x = 55
    gap = 12
    cell_width = 126
    for generation in range(8):
        x = start_x + generation * (cell_width + gap)
        if generation < 2:
            css = "fixed-box"
            role = "pre-shock"
        elif generation == 2:
            css = "shock-box"
            role = "imposed failure"
        else:
            css = "post-box"
            role = "post-shock"
        svg.rect(x, 378, cell_width, 76, class_=css, rx=8)
        svg.text(x + cell_width / 2, 406, f"Generation {generation}", class_="label", anchor="middle")
        svg.text(x + cell_width / 2, 431, role, class_="small", anchor="middle")
    svg.text(
        start_x + 2 * (cell_width + gap) + cell_width / 2,
        478,
        "receiver-ingress symbol blackout; identical for both policies",
        class_="small",
        anchor="middle",
    )

    svg.rect(55, 525, 1090, 105, class_="panel", rx=10)
    svg.text(82, 557, "23 complete randomized blocks", class_="heading")
    svg.text(82, 583, "2 treatments × 23 = 46 fresh VM-pair lifecycles", class_="small")
    svg.text(430, 557, "Primary outcome", class_="heading")
    svg.text(430, 583, "Σ(initial + emitted repair), generations 3–7", class_="small")
    svg.text(780, 557, "Evidence boundary", class_="heading")
    svg.text(780, 583, "freeze archive + hash before outcome analysis", class_="small")
    svg.text(780, 607, "delivery miss ⇒ efficiency interpretation inconclusive", class_="small")
    return svg.finish(
        "Raw-host post-shock efficiency study design",
        "The same frozen binary selects either fixed-class-aware or biological-adaptive at runtime. Both receive an imposed failure at generation two. Generations three through seven form the primary post-shock window in 23 paired complete blocks.",
    )


def legend(svg: Svg, entries: list[tuple[str, str]], y: float) -> None:
    x = 58
    for label, color in entries:
        svg.line(x, y, x + 28, y, style=f"stroke:{color};stroke-width:3")
        svg.circle(x + 14, y, 4, style=f"fill:{color}")
        svg.text(x + 38, y + 4, label, class_="small")
        x += 235


def render_pilot_adaptation(pilot: dict) -> str:
    svg = Svg.create(1200, 560)
    svg.text(55, 45, "Pilot causal response after the imposed failure", class_="title")
    svg.text(
        55, 72,
        "Protection factors are plans; generation 2 is the common perturbation, not a policy result",
        class_="subtitle",
    )
    legend(svg, [
        ("fixed-minimum", POLICY_COLORS["fixed-minimum"]),
        ("fixed-class-aware", POLICY_COLORS["fixed-class-aware"]),
        ("biological-adaptive", POLICY_COLORS["biological-adaptive"]),
    ], 102)
    factors = pilot["protection_factors"]
    panel_width = 342
    panel_gap = 38
    plot_top = 155
    plot_bottom = 450
    for class_index, class_name in enumerate(CLASS_NAMES):
        left = 58 + class_index * (panel_width + panel_gap)
        right = left + panel_width
        svg.text(left, 137, f"{class_name} protection factor", class_="heading")
        for tick in (1.0, 2.0, 3.0, 4.0):
            y = scale(tick, 0.8, 4.2, plot_bottom, plot_top)
            svg.line(left, y, right, y, class_="grid")
            svg.text(left - 8, y + 4, fmt(tick), class_="tiny", anchor="end")
        shock_x = scale(2, 0, 7, left + 15, right - 15)
        svg.rect(shock_x - 18, plot_top, 36, plot_bottom - plot_top, fill="#FEF3F2")
        svg.line(shock_x, plot_top, shock_x, plot_bottom, class_="shock-line")
        for generation in range(8):
            x = scale(generation, 0, 7, left + 15, right - 15)
            svg.text(x, plot_bottom + 23, generation, class_="tiny", anchor="middle")
        svg.line(left, plot_bottom, right, plot_bottom, class_="axis")
        for policy, generations in factors.items():
            points = [
                (
                    scale(index, 0, 7, left + 15, right - 15),
                    scale(
                        number(plan[class_index], "protection factor"),
                        0.8, 4.2, plot_bottom, plot_top,
                    ),
                )
                for index, plan in enumerate(generations)
            ]
            color = POLICY_COLORS[policy]
            svg.polyline(
                points,
                style=f"stroke:{color};stroke-width:2.5;fill:none",
            )
            for x, y in points:
                svg.circle(x, y, 3.7, style=f"fill:{color}")
        svg.text(
            (left + right) / 2,
            plot_bottom + 47,
            "Generation index",
            class_="small",
            anchor="middle",
        )
    svg.text(55, 535, "Published descriptive pilot · two blocks · no superiority claim", class_="small")
    return svg.finish(
        "Pilot causal protection response",
        "Three panels show critical, important and elastic protection factors across generations zero through seven for the three pilot policies. Biological-adaptive increases critical and important protection in the plan after the imposed generation-two failure while fixed policies remain unchanged.",
    )


def render_pilot_efficiency(pilot: dict) -> str:
    svg = Svg.create(1200, 545)
    svg.text(55, 45, "Pilot post-shock wire-symbol composition", class_="title")
    svg.text(
        55, 72,
        "Five scheduled generations (3–7); every cell delivered 5/5 critical segments before deadline",
        class_="subtitle",
    )
    svg.rect(55, 94, 18, 18, class_="initial")
    svg.text(82, 108, "initial symbols", class_="small")
    svg.rect(205, 94, 18, 18, class_="repair")
    svg.text(232, 108, "emitted repair symbols", class_="small")
    blocks = pilot["post_shock_blocks"]
    max_total = max(
        block[policy]["wire_symbol_datagrams"]
        for block in blocks for policy in POLICIES
    )
    plot_left = 245
    plot_right = 1115
    domain_max = max_total * 1.12
    row_y = 165
    for block in blocks:
        svg.text(55, row_y - 27, f"Block {block['block']}", class_="heading")
        for policy_index, policy in enumerate(POLICIES):
            y = row_y + policy_index * 72
            label = policy.replace("-", " ")
            svg.text(220, y + 25, label, class_="label", anchor="end")
            outcome = block[policy]
            initial = outcome["initial_symbols"]
            repair = outcome["repair_symbols_emitted"]
            initial_width = scale(initial, 0, domain_max, 0, plot_right - plot_left)
            repair_width = scale(repair, 0, domain_max, 0, plot_right - plot_left)
            svg.rect(plot_left, y, initial_width, 38, class_="initial")
            svg.rect(plot_left + initial_width, y, repair_width, 38, class_="repair")
            svg.text(
                plot_left + initial_width + repair_width + 10,
                y + 25,
                outcome["wire_symbol_datagrams"],
                class_="value",
            )
        row_y += 174
    axis_y = 488
    svg.line(plot_left, axis_y, plot_right, axis_y, class_="axis")
    for tick in range(0, 901, 200):
        x = scale(tick, 0, domain_max, plot_left, plot_right)
        if x <= plot_right:
            svg.line(x, axis_y - 4, x, axis_y + 4, class_="axis")
            svg.text(x, axis_y + 22, tick, class_="tiny", anchor="middle")
    svg.text((plot_left + plot_right) / 2, 532, "Wire-symbol datagrams", class_="small", anchor="middle")
    return svg.finish(
        "Pilot post-shock wire-symbol composition",
        "Stacked bars show initial and emitted repair symbols for fixed-class-aware and biological-adaptive in each of the two descriptive pilot blocks. Totals are shown at each bar end. All four cells delivered five of five post-shock critical segments before receiver deadlines.",
    )


def validate_analysis(value: dict) -> None:
    if value.get("schema") != ANALYSIS_SCHEMA or \
            value.get("study_id") != STUDY_ID or \
            value.get("complete_blocks") != 23 or value.get("lifecycles") != 46:
        raise VisualError("unexpected confirmatory analysis identity")
    if not isinstance(value.get("source_commit"), str) or \
            len(value["source_commit"]) != 40:
        raise VisualError("analysis source commit is invalid")
    for key in ("runtime_binary_sha256", "frozen_evidence_archive_sha256"):
        if not isinstance(value.get(key), str) or len(value[key]) != 64:
            raise VisualError(f"analysis {key} is invalid")
    primary = value.get("primary_analysis", {})
    if primary.get("contrast") != \
            "biological-adaptive-minus-fixed-class-aware" or \
            primary.get("shock_generation_excluded") != 2 or \
            primary.get("post_shock_generation_indices") != [3, 4, 5, 6, 7] or \
            primary.get("minimum_relevant_effect_wire_symbols") != 50:
        raise VisualError("analysis primary semantics changed")
    interval = primary.get("confidence_interval_95_percent")
    if not isinstance(interval, list) or len(interval) != 2:
        raise VisualError("analysis confidence interval is invalid")
    number(interval[0], "confidence interval lower")
    number(interval[1], "confidence interval upper")
    blocks = value.get("block_results")
    if not isinstance(blocks, list) or len(blocks) != 23 or \
            [block.get("block") for block in blocks] != list(range(1, 24)):
        raise VisualError("analysis does not contain 23 registered blocks")
    lifecycles = value.get("per_lifecycle_evidence")
    if not isinstance(lifecycles, list) or len(lifecycles) != 46:
        raise VisualError("analysis does not contain 46 lifecycles")
    by_block: dict[int, set[str]] = defaultdict(set)
    for lifecycle in lifecycles:
        block = integer(lifecycle.get("block"), "lifecycle block")
        policy = lifecycle.get("policy")
        by_block[block].add(policy)
        generations = lifecycle.get("generations")
        if policy not in POLICIES or not isinstance(generations, list) or \
                [item.get("index") for item in generations] != list(range(8)):
            raise VisualError("lifecycle policy or generation evidence is invalid")
        for generation in generations:
            factors = generation.get("protection_factors")
            if not isinstance(factors, list) or len(factors) != 3:
                raise VisualError("generation protection factors are invalid")
            for factor in factors:
                number(factor, "generation protection factor")
    if set(by_block) != set(range(1, 24)) or \
            any(policies != set(POLICIES) for policies in by_block.values()):
        raise VisualError("analysis is not 23 complete policy pairs")


def render_primary_contrast(analysis: dict) -> str:
    svg = Svg.create(1100, 760)
    svg.text(55, 45, "Primary paired post-shock contrast", class_="title")
    svg.text(
        55, 72,
        "biological-adaptive − fixed-class-aware wire-symbol datagrams; negative favors adaptive efficiency",
        class_="subtitle",
    )
    blocks = analysis["block_results"]
    contrasts = [number(
        block["primary_contrast_wire_symbols"], "block contrast"
    ) for block in blocks]
    primary = analysis["primary_analysis"]
    mean = number(primary["mean_contrast_wire_symbols"], "mean contrast")
    interval = [number(value, "confidence interval") for value in
                primary["confidence_interval_95_percent"]]
    domain_min, domain_max = nice_domain(
        [*contrasts, *interval, 0.0, -50.0]
    )
    plot_left = 180
    plot_right = 1030
    plot_top = 115
    row_gap = 22
    zero_x = scale(0, domain_min, domain_max, plot_left, plot_right)
    mre_x = scale(-50, domain_min, domain_max, plot_left, plot_right)
    svg.line(zero_x, plot_top - 8, zero_x, 650, class_="reference")
    svg.line(mre_x, plot_top - 8, mre_x, 650, class_="mre")
    svg.text(zero_x, 96, "no difference", class_="tiny", anchor="middle")
    svg.text(mre_x, 96, "registered −50", class_="tiny", anchor="middle")
    for index, contrast in enumerate(contrasts):
        y = plot_top + index * row_gap
        svg.text(145, y + 4, f"Block {index + 1}", class_="tiny", anchor="end")
        svg.line(plot_left, y, plot_right, y, class_="grid")
        x = scale(contrast, domain_min, domain_max, plot_left, plot_right)
        svg.circle(x, y, 5, class_="neutral")
        svg.text(x + (9 if x < plot_right - 35 else -9), y + 4, fmt(contrast), class_="tiny", anchor="start" if x < plot_right - 35 else "end")
    summary_y = 650
    low_x = scale(interval[0], domain_min, domain_max, plot_left, plot_right)
    high_x = scale(interval[1], domain_min, domain_max, plot_left, plot_right)
    mean_x = scale(mean, domain_min, domain_max, plot_left, plot_right)
    svg.text(145, summary_y + 4, "Mean (95% CI)", class_="label", anchor="end")
    svg.line(low_x, summary_y, high_x, summary_y, class_="mean")
    svg.line(low_x, summary_y - 8, low_x, summary_y + 8, class_="mean")
    svg.line(high_x, summary_y - 8, high_x, summary_y + 8, class_="mean")
    svg.circle(mean_x, summary_y, 7, class_="mean")
    svg.text(
        mean_x,
        summary_y + 28,
        f"{fmt(mean)} [{fmt(interval[0])}, {fmt(interval[1])}]",
        class_="small",
        anchor="middle",
    )
    axis_y = 706
    svg.line(plot_left, axis_y, plot_right, axis_y, class_="axis")
    for index in range(6):
        value = domain_min + (domain_max - domain_min) * index / 5
        x = scale(value, domain_min, domain_max, plot_left, plot_right)
        svg.line(x, axis_y - 4, x, axis_y + 4, class_="axis")
        svg.text(x, axis_y + 22, fmt(value), class_="tiny", anchor="middle")
    svg.text((plot_left + plot_right) / 2, 752, "Paired contrast (wire-symbol datagrams)", class_="small", anchor="middle")
    return svg.finish(
        "Primary paired post-shock contrast",
        "All 23 registered block contrasts appear in block order. A summary interval shows the mean and two-sided 95 percent Student-t confidence interval. Reference lines mark zero and the registered minus-50-symbol minimum relevant effect.",
    )


def lifecycle_by_policy(analysis: dict) -> dict[str, list[dict]]:
    grouped = {policy: [] for policy in POLICIES}
    for lifecycle in analysis["per_lifecycle_evidence"]:
        grouped[lifecycle["policy"]].append(lifecycle)
    return grouped


def render_generation_protection(analysis: dict) -> str:
    svg = Svg.create(1200, 680)
    svg.text(55, 45, "Protection plan across the causal timeline", class_="title")
    svg.text(
        55, 72,
        "Mean and full observed range across 23 lifecycles per policy; generation 2 is the imposed failure",
        class_="subtitle",
    )
    legend(svg, [
        ("fixed-class-aware", POLICY_COLORS["fixed-class-aware"]),
        ("biological-adaptive", POLICY_COLORS["biological-adaptive"]),
    ], 102)
    grouped = lifecycle_by_policy(analysis)
    panel_width = 342
    panel_gap = 38
    plot_top = 155
    plot_bottom = 545
    all_factors = [
        number(factor, "protection factor")
        for lifecycles in grouped.values() for lifecycle in lifecycles
        for generation in lifecycle["generations"]
        for factor in generation["protection_factors"]
    ]
    y_min, y_max = nice_domain(all_factors)
    for class_index, class_name in enumerate(CLASS_NAMES):
        left = 58 + class_index * (panel_width + panel_gap)
        right = left + panel_width
        svg.text(left, 137, class_name, class_="heading")
        for index in range(5):
            tick = y_min + (y_max - y_min) * index / 4
            y = scale(tick, y_min, y_max, plot_bottom, plot_top)
            svg.line(left, y, right, y, class_="grid")
            svg.text(left - 8, y + 4, fmt(tick), class_="tiny", anchor="end")
        shock_x = scale(2, 0, 7, left + 15, right - 15)
        svg.rect(shock_x - 18, plot_top, 36, plot_bottom - plot_top, fill="#FEF3F2")
        svg.line(shock_x, plot_top, shock_x, plot_bottom, class_="shock-line")
        for policy in POLICIES:
            means = []
            ranges = []
            for generation_index in range(8):
                values = [number(
                    lifecycle["generations"][generation_index]
                    ["protection_factors"][class_index],
                    "protection factor",
                ) for lifecycle in grouped[policy]]
                means.append(statistics.mean(values))
                ranges.append((min(values), max(values)))
            upper = []
            lower = []
            for generation_index, (_, high) in enumerate(ranges):
                x = scale(generation_index, 0, 7, left + 15, right - 15)
                upper.append((x, scale(high, y_min, y_max, plot_bottom, plot_top)))
            for generation_index in reversed(range(8)):
                low = ranges[generation_index][0]
                x = scale(generation_index, 0, 7, left + 15, right - 15)
                lower.append((x, scale(low, y_min, y_max, plot_bottom, plot_top)))
            svg.polyline(
                [*upper, *lower, upper[0]],
                style=(
                    f"fill:{POLICY_COLORS[policy]};fill-opacity:.10;"
                    "stroke:none"
                ),
            )
            points = [
                (
                    scale(index, 0, 7, left + 15, right - 15),
                    scale(value, y_min, y_max, plot_bottom, plot_top),
                )
                for index, value in enumerate(means)
            ]
            svg.polyline(
                points,
                style=(
                    f"stroke:{POLICY_COLORS[policy]};"
                    "stroke-width:2.5;fill:none"
                ),
            )
            for x, y in points:
                svg.circle(x, y, 3.5, style=f"fill:{POLICY_COLORS[policy]}")
        for generation in range(8):
            x = scale(generation, 0, 7, left + 15, right - 15)
            svg.text(x, plot_bottom + 23, generation, class_="tiny", anchor="middle")
        svg.line(left, plot_bottom, right, plot_bottom, class_="axis")
        svg.text((left + right) / 2, plot_bottom + 47, "Generation index", class_="small", anchor="middle")
    svg.text(55, 646, "Shaded ranges remain visible even if policy plans vary across lifecycles.", class_="small")
    return svg.finish(
        "Protection plan across the causal timeline",
        "Three panels show critical, important and elastic protection factors by generation for both confirmatory treatments. Lines are policy means and shaded areas are the full observed ranges across all lifecycles. Generation two is marked as the common imposed failure.",
    )


def render_wire_composition(analysis: dict) -> str:
    svg = Svg.create(1100, 560)
    svg.text(55, 45, "Post-shock wire-symbol composition", class_="title")
    svg.text(
        55, 72,
        "Policy means over five scheduled generations; dots retain every lifecycle total",
        class_="subtitle",
    )
    svg.rect(55, 94, 18, 18, class_="initial")
    svg.text(82, 108, "initial symbols", class_="small")
    svg.rect(205, 94, 18, 18, class_="repair")
    svg.text(232, 108, "emitted repair symbols", class_="small")
    grouped = lifecycle_by_policy(analysis)
    totals = [number(
        lifecycle["post_shock"]["wire_symbol_datagrams"], "wire total"
    ) for lifecycles in grouped.values() for lifecycle in lifecycles]
    domain_max = max(totals) * 1.12 if totals else 1.0
    plot_left = 265
    plot_right = 1030
    rows = {"fixed-class-aware": 205, "biological-adaptive": 385}
    for policy, y in rows.items():
        lifecycles = grouped[policy]
        initial = statistics.mean(number(
            lifecycle["post_shock"]["initial_symbols"], "initial symbols"
        ) for lifecycle in lifecycles)
        repair = statistics.mean(number(
            lifecycle["post_shock"]["repair_symbols_emitted"], "repair symbols"
        ) for lifecycle in lifecycles)
        initial_width = scale(initial, 0, domain_max, 0, plot_right - plot_left)
        repair_width = scale(repair, 0, domain_max, 0, plot_right - plot_left)
        svg.text(235, y + 25, policy.replace("-", " "), class_="label", anchor="end")
        svg.rect(plot_left, y, initial_width, 42, class_="initial")
        svg.rect(plot_left + initial_width, y, repair_width, 42, class_="repair")
        mean_total = initial + repair
        svg.text(
            plot_left + initial_width + repair_width + 10,
            y + 27,
            f"mean {fmt(mean_total)}",
            class_="value",
        )
        dot_y = y + 78
        svg.line(plot_left, dot_y, plot_right, dot_y, class_="grid")
        for lifecycle in lifecycles:
            total = number(
                lifecycle["post_shock"]["wire_symbol_datagrams"], "wire total"
            )
            x = scale(total, 0, domain_max, plot_left, plot_right)
            svg.circle(
                x, dot_y, 4,
                style=(
                    f"fill:{POLICY_COLORS[policy]};fill-opacity:.7"
                ),
            )
    axis_y = 500
    svg.line(plot_left, axis_y, plot_right, axis_y, class_="axis")
    for index in range(6):
        value = domain_max * index / 5
        x = scale(value, 0, domain_max, plot_left, plot_right)
        svg.line(x, axis_y - 4, x, axis_y + 4, class_="axis")
        svg.text(x, axis_y + 22, fmt(value), class_="tiny", anchor="middle")
    svg.text((plot_left + plot_right) / 2, 548, "Wire-symbol datagrams", class_="small", anchor="middle")
    return svg.finish(
        "Post-shock wire-symbol composition",
        "Two stacked bars show mean initial and emitted repair symbols for the fixed-class-aware and biological-adaptive policies. Individual lifecycle totals are retained as dots beneath each bar.",
    )


def render_delivery_guardrail(analysis: dict) -> str:
    svg = Svg.create(1100, 410)
    svg.text(55, 45, "Post-shock critical-delivery guardrail", class_="title")
    svg.text(
        55, 72,
        "Critical generations delivered before receiver-local deadline, out of five scheduled generations",
        class_="subtitle",
    )
    blocks = analysis["block_results"]
    plot_left = 235
    plot_right = 1035
    cell_width = (plot_right - plot_left) / 23
    rows = [
        (
            "fixed-class-aware",
            "fixed_class_aware_critical_before_deadline",
            175,
        ),
        (
            "biological-adaptive",
            "biological_adaptive_critical_before_deadline",
            275,
        ),
    ]
    for block_index in range(23):
        x = plot_left + (block_index + 0.5) * cell_width
        svg.text(x, 125, block_index + 1, class_="tiny", anchor="middle")
    svg.text(plot_left - 12, 125, "Block", class_="small", anchor="end")
    for policy, key, y in rows:
        svg.text(plot_left - 12, y + 4, policy.replace("-", " "), class_="label", anchor="end")
        for block_index, block in enumerate(blocks):
            value = integer(block.get(key), "delivery guardrail count")
            x = plot_left + (block_index + 0.5) * cell_width
            svg.circle(x, y, 13, class_="success" if value == 5 else "failure")
            svg.text(x, y + 4, value, class_="value", anchor="middle", fill="#FFFFFF")
    guardrail = analysis.get("delivery_guardrail", {})
    passed = guardrail.get("passed") is True
    svg.rect(55, 335, 980, 48, class_="post-box" if passed else "shock-box", rx=8)
    svg.text(
        75, 365,
        "Guardrail passed: efficiency interpretation permitted"
        if passed else
        "Guardrail failed: efficiency interpretation is inconclusive",
        class_="heading",
    )
    return svg.finish(
        "Post-shock critical-delivery guardrail",
        "A two-row grid shows the number of post-shock critical generations delivered before receiver-local deadlines for each policy and each of the 23 registered blocks. Five is a passing cell; any lower count fails the study-wide efficiency guardrail.",
    )


def registered_filenames(visual: dict) -> dict[str, str]:
    return {figure["id"]: figure["file"] for figure in visual["figures"]}


def render_repository_visuals(
    pilot: dict,
    study: dict,
    output_dir: Path,
) -> list[Path]:
    outputs = {
        "raw-host-post-shock-study-design-v1.svg": render_study_design(study),
        "raw-host-policy-pilot-v1-causal-response.svg":
            render_pilot_adaptation(pilot),
        "raw-host-policy-pilot-v1-efficiency-signal.svg":
            render_pilot_efficiency(pilot),
    }
    paths = []
    for filename, content in outputs.items():
        path = output_dir / filename
        write_lf(path, content)
        paths.append(path)
    return paths


def render_analysis_visuals(
    analysis: dict,
    analysis_path: Path,
    visual: dict,
    visual_path: Path,
    output_dir: Path,
) -> list[Path]:
    filenames = registered_filenames(visual)
    outputs = {
        filenames["primary-paired-contrast"]:
            render_primary_contrast(analysis),
        filenames["generation-protection-response"]:
            render_generation_protection(analysis),
        filenames["post-shock-wire-composition"]:
            render_wire_composition(analysis),
        filenames["delivery-guardrail"]:
            render_delivery_guardrail(analysis),
    }
    paths = []
    for filename, content in outputs.items():
        path = output_dir / filename
        write_lf(path, content)
        paths.append(path)
    manifest = {
        "schema": "aurora-raw-host-post-shock-visual-manifest-v1",
        "study_id": analysis["study_id"],
        "analysis_sha256": sha256_file(analysis_path),
        "source_commit": analysis["source_commit"],
        "runtime_binary_sha256": analysis["runtime_binary_sha256"],
        "frozen_evidence_archive_sha256": analysis[
            "frozen_evidence_archive_sha256"
        ],
        "visualization_spec_sha256": sha256_file(visual_path),
        "figure_sha256": {
            path.name: sha256_file(path) for path in sorted(paths)
        },
    }
    manifest_path = output_dir / "raw-host-post-shock-visual-manifest.json"
    write_lf(
        manifest_path,
        json.dumps(manifest, indent=2, sort_keys=True, allow_nan=False),
    )
    paths.append(manifest_path)
    return paths


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--pilot-input", type=Path, default=DEFAULT_PILOT_INPUT)
    result.add_argument("--study-spec", type=Path, default=DEFAULT_STUDY_SPEC)
    result.add_argument("--visual-spec", type=Path, default=DEFAULT_VISUAL_SPEC)
    result.add_argument("--analysis", type=Path)
    result.add_argument("--output-dir", type=Path, required=True)
    result.add_argument(
        "--results-only",
        action="store_true",
        help="render only registered confirmatory figures (requires --analysis)",
    )
    return result


def main(argv: Iterable[str] | None = None) -> int:
    args = parser().parse_args(argv)
    pilot = load_json(args.pilot_input)
    study = load_json(args.study_spec)
    visual = load_json(args.visual_spec)
    validate_pilot(pilot)
    validate_study(study, visual)
    if args.results_only and args.analysis is None:
        raise VisualError("--results-only requires --analysis")
    outputs = []
    if not args.results_only:
        outputs.extend(render_repository_visuals(pilot, study, args.output_dir))
    if args.analysis is not None:
        analysis = load_json(args.analysis)
        validate_analysis(analysis)
        outputs.extend(render_analysis_visuals(
            analysis,
            args.analysis,
            visual,
            args.visual_spec,
            args.output_dir,
        ))
    print(json.dumps({
        "output_dir": str(args.output_dir),
        "files": [path.name for path in outputs],
    }, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except VisualError as error:
        print(f"raw-host policy visuals: {error}")
        raise SystemExit(1)
