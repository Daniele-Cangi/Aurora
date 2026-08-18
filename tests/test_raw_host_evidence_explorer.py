import copy
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import render_raw_host_evidence_explorer as explorer  # noqa: E402


class RawHostEvidenceExplorerTests(unittest.TestCase):
    def setUp(self):
        self.input_path = (
            ROOT / "benchmarks" /
            "raw_host_policy_pilot_v1_explorer_input.json"
        )
        self.visual_input_path = (
            ROOT / "benchmarks" /
            "raw_host_policy_pilot_v1_visualization_input.json"
        )
        self.tracked_output = (
            ROOT / "docs" / "raw-host-policy-pilot-v1-explorer.html"
        )

    def test_explorer_input_matches_frozen_visual_contract(self):
        value = explorer.load_json(self.input_path)
        visual = explorer.load_json(self.visual_input_path)
        explorer.validate_input(value)

        self.assertEqual(value["pilot_id"], visual["pilot_id"])
        self.assertEqual(
            value["provenance"]["experimental_source"],
            visual["experimental_source"],
        )
        self.assertEqual(
            value["provenance"]["evidence_archive_sha256"],
            visual["evidence_archive_sha256"],
        )
        self.assertEqual(
            value["provenance"]["analysis_json_sha256"],
            visual["analysis_json_sha256"],
        )
        self.assertEqual(value["windows"], visual["windows"])
        self.assertEqual(
            {
                policy: evidence["protection_factors"]
                for policy, evidence in value["policies"].items()
            },
            visual["protection_factors"],
        )

        for block_index, visual_block in enumerate(
            visual["post_shock_blocks"]
        ):
            for policy in (
                "fixed-class-aware",
                "biological-adaptive",
            ):
                explorer_policy = value["post_shock"]["policies"][policy]
                visual_policy = visual_block[policy]
                self.assertEqual(
                    explorer_policy["critical_before_deadline"][block_index],
                    visual_policy["critical_before_deadline"],
                )
                self.assertEqual(
                    explorer_policy["initial_symbols"][block_index],
                    visual_policy["initial_symbols"],
                )
                self.assertEqual(
                    explorer_policy["repair_emitted"][block_index],
                    visual_policy["repair_symbols_emitted"],
                )
                self.assertEqual(
                    explorer_policy["wire_symbols"][block_index],
                    visual_policy["wire_symbol_datagrams"],
                )

        for claim, expected in visual["claim_boundary"].items():
            self.assertEqual(value["claims"][claim], expected)

    def test_tracked_explorer_is_deterministic_and_offline(self):
        value = explorer.load_json(self.input_path)
        explorer.validate_input(value)
        with tempfile.TemporaryDirectory() as name:
            output = Path(name) / "explorer.html"
            explorer.write_lf(
                output,
                explorer.render_explorer(
                    value,
                    explorer.sha256_file(self.input_path),
                ),
            )
            self.assertEqual(output.read_bytes(), self.tracked_output.read_bytes())

        html = self.tracked_output.read_text(encoding="utf-8")
        self.assertIn("Aurora Evidence Explorer", html)
        self.assertIn('id="response-chart"', html)
        self.assertIn('id="post-shock-bars"', html)
        self.assertIn('id="provenance-grid"', html)
        self.assertNotIn("fetch(", html)
        self.assertNotIn("XMLHttpRequest", html)
        self.assertNotIn("WebSocket", html)
        self.assertNotIn("<script src=", html)
        self.assertLess(len(html.encode("utf-8")), 1024 * 1024)

    def test_local_release_analysis_matches_declared_hash_when_present(self):
        analysis_path = (
            ROOT / "raw-host-evidence" /
            "raw-host-policy-pilot-v1-study-v4-analysis.json"
        )
        if not analysis_path.is_file():
            self.skipTest("release analysis is not part of a clean checkout")
        value = explorer.load_json(self.input_path)
        analysis = json.loads(analysis_path.read_text(encoding="utf-8"))
        digest = hashlib.sha256(analysis_path.read_bytes()).hexdigest()
        self.assertEqual(digest, value["provenance"]["analysis_json_sha256"])
        self.assertEqual(
            analysis["cell_means_across_two_blocks"],
            value["condition_means_across_two_blocks"],
        )
        self.assertEqual(
            analysis["experimental_source_commit"],
            value["provenance"]["experimental_source"],
        )
        self.assertEqual(
            analysis["integrity"]["runtime_binary_sha256"],
            value["provenance"]["runtime_binary_sha256"],
        )
        self.assertEqual(analysis["delivery"], {
            **value["delivery"],
            "regime-change-v1": {
                **value["delivery"]["regime-change-v1"],
                "generation_windows_all_policies_all_blocks": analysis[
                    "delivery"
                ]["regime-change-v1"][
                    "generation_windows_all_policies_all_blocks"
                ],
            },
        })

        regression = analysis["causal_adaptation_regression"]
        for policy in ("fixed-minimum", "fixed-class-aware"):
            expected = regression[policy]["protection_factors_all_generations"]
            self.assertTrue(all(
                factors == expected
                for factors in value["policies"][policy]["protection_factors"]
            ))
        biological = regression["biological-adaptive"]
        self.assertEqual(
            [entry["factors"] for entry in biological],
            value["policies"]["biological-adaptive"]["protection_factors"],
        )
        for expected, explorer_state in zip(
            biological,
            value["policies"]["biological-adaptive"]["adaptive_state"],
        ):
            self.assertEqual(expected["generation"], explorer_state["generation"])
            self.assertEqual(expected["state_at_plan"], explorer_state["at_plan"])
            if expected["generation"] == 2:
                self.assertEqual(
                    expected["terminal_outcome"],
                    explorer_state["terminal_outcome"],
                )
                self.assertEqual(
                    expected["state_after_terminal"],
                    explorer_state["after_terminal"],
                )

        analysis_post_shock = {
            row["policy"]: row
            for row in analysis["post_shock_generations_3_7"]
        }
        for policy, explorer_values in value["post_shock"]["policies"].items():
            expected = analysis_post_shock[policy]
            self.assertEqual(
                explorer_values["critical_before_deadline"],
                [int(item.split("/")[0]) for item in expected[
                    "critical_delivery_by_block"
                ]],
            )
            for explorer_key, analysis_key in (
                ("initial_symbols", "initial_symbols_by_block"),
                ("repair_requested", "repair_requested_by_block"),
                ("repair_emitted", "repair_emitted_by_block"),
                ("wire_symbols", "wire_symbols_by_block"),
            ):
                self.assertEqual(
                    explorer_values[explorer_key],
                    expected[analysis_key],
                )

    def test_superiority_or_clock_drift_is_rejected(self):
        value = explorer.load_json(self.input_path)
        superiority = copy.deepcopy(value)
        superiority["claims"]["policy_superiority"] = True
        with self.assertRaises(explorer.ExplorerError):
            explorer.validate_input(superiority)

        wrong_clock = copy.deepcopy(value)
        wrong_clock["deadline_semantics"]["clock"] = "sender-relative"
        with self.assertRaises(explorer.ExplorerError):
            explorer.validate_input(wrong_clock)

        unchanged_adaptive = copy.deepcopy(value)
        unchanged_adaptive["policies"]["biological-adaptive"][
            "protection_factors"
        ][3] = unchanged_adaptive["policies"]["biological-adaptive"][
            "protection_factors"
        ][2]
        with self.assertRaises(explorer.ExplorerError):
            explorer.validate_input(unchanged_adaptive)


if __name__ == "__main__":
    unittest.main()
