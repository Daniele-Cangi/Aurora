import copy
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import render_raw_host_policy_visuals as visuals  # noqa: E402


def synthetic_analysis() -> dict:
    lifecycle_evidence = []
    block_results = []
    contrasts = []
    for block in range(1, 24):
        fixed_total = 800
        biological_total = 744 + (block % 4) * 4
        contrast = biological_total - fixed_total
        contrasts.append(contrast)
        block_results.append({
            "block": block,
            "fixed_class_aware_wire_symbols": fixed_total,
            "biological_adaptive_wire_symbols": biological_total,
            "primary_contrast_wire_symbols": contrast,
            "fixed_class_aware_critical_before_deadline": 5,
            "biological_adaptive_critical_before_deadline": 5,
        })
        for sequence_in_block, policy in enumerate(visuals.POLICIES):
            if policy == "fixed-class-aware":
                initial = 365
                repair = 435
                plans = [[2.5, 1.5, 1.1] for _ in range(8)]
            else:
                initial = 490
                repair = biological_total - initial
                plans = [
                    [2.5, 1.5, 1.25],
                    [2.5, 1.5, 1.25],
                    [2.5, 1.5, 1.25],
                    [4.0, 2.325, 1.25],
                    [4.0, 2.325, 1.25],
                    [4.0, 2.325, 1.25],
                    [2.6, 1.55, 1.25],
                    [2.58, 1.53, 1.25],
                ]
            generations = []
            for index, plan in enumerate(plans):
                generations.append({
                    "index": index,
                    "delivered": 0 if index == 2 else 1,
                    "terminal_failure": 1 if index == 2 else 0,
                    "critical_before_deadline": 0 if index == 2 else 1,
                    "source_symbols": 40,
                    "initial_symbols": initial // 5,
                    "repair_symbols_requested": repair // 5,
                    "repair_symbols_emitted": repair // 5,
                    "wire_symbol_datagrams": (initial + repair) // 5,
                    "protection_factors": plan,
                    "adaptive_state_present":
                        1 if policy == "biological-adaptive" else 0,
                    "adaptive_success_count_at_plan": max(index - 1, 0),
                    "adaptive_failure_count_at_plan": 1 if index >= 3 else 0,
                    "adaptive_panic_boost_at_plan":
                        max(5 - index, 0) if 3 <= index <= 5 else 0,
                })
            lifecycle_evidence.append({
                "sequence": (block - 1) * 2 + sequence_in_block + 1,
                "block": block,
                "run_id": f"test-b{block:02d}-{sequence_in_block}",
                "policy": policy,
                "runtime_binary_sha256": "a" * 64,
                "post_shock": {
                    "scheduled_generations": 5,
                    "critical_before_deadline": 5,
                    "delivered": 5,
                    "source_symbols": 200,
                    "initial_symbols": initial,
                    "repair_symbols_requested": repair,
                    "repair_symbols_emitted": repair,
                    "wire_symbol_datagrams": initial + repair,
                },
                "whole_lifecycle": {
                    "terminal_feedback_rtt_mean_us": 150000,
                    "terminal_feedback_retry_rounds": 10,
                },
                "generations": generations,
            })
    mean = sum(contrasts) / len(contrasts)
    return {
        "schema": visuals.ANALYSIS_SCHEMA,
        "study_id": visuals.STUDY_ID,
        "source_commit": "0123456789abcdef0123456789abcdef01234567",
        "runtime_binary_sha256": "a" * 64,
        "frozen_evidence_archive": "frozen.tar.gz",
        "frozen_evidence_archive_sha256": "b" * 64,
        "complete_blocks": 23,
        "lifecycles": 46,
        "primary_analysis": {
            "outcome": (
                "post-shock-wire-symbol-datagrams-over-scheduled-generations"
            ),
            "contrast": "biological-adaptive-minus-fixed-class-aware",
            "shock_generation_excluded": 2,
            "post_shock_generation_indices": [3, 4, 5, 6, 7],
            "mean_contrast_wire_symbols": mean,
            "mean_contrast_wire_symbols_per_scheduled_generation": mean / 5,
            "standard_deviation_wire_symbols": 5.0,
            "standard_error_wire_symbols": 1.0,
            "t_statistic": mean,
            "degrees_of_freedom": 22,
            "critical_t_two_sided_alpha_0_05": 2.073873,
            "confidence_interval_95_percent": [mean - 2.1, mean + 2.1],
            "confidence_interval_excludes_zero_in_efficiency_direction": True,
            "minimum_relevant_effect_wire_symbols": 50,
            "point_estimate_meets_registered_relevance": True,
        },
        "delivery_guardrail": {
            "rule": "all five post-shock generations",
            "passed": True,
        },
        "classification": (
            "efficiency-advantage-statistically-supported-and-practically-relevant"
        ),
        "delivery_superiority_claim": False,
        "block_results": block_results,
        "secondary_descriptive_contrasts": {},
        "per_lifecycle_evidence": lifecycle_evidence,
        "claims": {
            "confirmatory": True,
            "calibrated_performance": False,
            "field_evidence": False,
            "delivery_superiority": False,
        },
    }


class RawHostPolicyVisualTests(unittest.TestCase):
    def setUp(self):
        self.pilot_path = (
            ROOT / "benchmarks" /
            "raw_host_policy_pilot_v1_visualization_input.json"
        )
        self.study_path = (
            ROOT / "benchmarks" /
            "gcp_raw_post_shock_efficiency_study_v1.json"
        )
        self.visual_path = (
            ROOT / "benchmarks" /
            "raw_host_post_shock_efficiency_visualization_v1.json"
        )

    def test_registered_visual_contract_and_repository_assets(self):
        pilot = visuals.load_json(self.pilot_path)
        study = visuals.load_json(self.study_path)
        visual = visuals.load_json(self.visual_path)
        visuals.validate_pilot(pilot)
        visuals.validate_study(study, visual)
        with tempfile.TemporaryDirectory() as name:
            outputs = visuals.render_repository_visuals(
                pilot, study, Path(name)
            )
            self.assertEqual(len(outputs), 3)
            for generated in outputs:
                ET.parse(generated)
                tracked = ROOT / "docs" / "images" / generated.name
                self.assertTrue(tracked.is_file())
                self.assertEqual(generated.read_bytes(), tracked.read_bytes())

    def test_confirmatory_figures_include_manifest_and_all_blocks(self):
        analysis = synthetic_analysis()
        visuals.validate_analysis(analysis)
        visual = visuals.load_json(self.visual_path)
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            analysis_path = directory / "analysis.json"
            with analysis_path.open(
                "w", encoding="utf-8", newline="\n"
            ) as stream:
                json.dump(analysis, stream, indent=2, sort_keys=True)
                stream.write("\n")
            outputs = visuals.render_analysis_visuals(
                analysis,
                analysis_path,
                visual,
                self.visual_path,
                directory,
            )
            self.assertEqual(len(outputs), 5)
            figure_paths = [path for path in outputs if path.suffix == ".svg"]
            self.assertEqual(len(figure_paths), 4)
            for path in figure_paths:
                ET.parse(path)
            primary = (
                directory / "raw-host-post-shock-primary-contrast.svg"
            ).read_text(encoding="utf-8")
            self.assertIn("Block 23", primary)
            self.assertIn("registered −50", primary)
            manifest = json.loads(
                (directory / "raw-host-post-shock-visual-manifest.json")
                .read_text(encoding="utf-8")
            )
            self.assertEqual(
                manifest["analysis_sha256"],
                hashlib.sha256(analysis_path.read_bytes()).hexdigest(),
            )
            self.assertEqual(len(manifest["figure_sha256"]), 4)
            self.assertEqual(manifest["source_commit"], analysis["source_commit"])

    def test_invalid_or_incomplete_analysis_is_rejected(self):
        analysis = synthetic_analysis()
        broken = copy.deepcopy(analysis)
        broken["per_lifecycle_evidence"].pop()
        with self.assertRaises(visuals.VisualError):
            visuals.validate_analysis(broken)


if __name__ == "__main__":
    unittest.main()
