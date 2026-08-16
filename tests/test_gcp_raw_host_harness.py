import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import gcp_raw_host_emulation as harness  # noqa: E402
import gcp_raw_host_matrix as raw_matrix  # noqa: E402
import gcp_raw_policy_pilot as policy_pilot  # noqa: E402
import gcp_raw_power_analysis as power_analysis  # noqa: E402
import summarize_gcp_raw_campaign as campaign  # noqa: E402


COMMIT = "0123456789abcdef0123456789abcdef01234567"


def config(temp: Path, run_id="test-run-001"):
    return harness.Config(
        project="aurora-raw-test-12345",
        run_id=run_id,
        sender_zone="us-east1-b",
        receiver_zone="us-west1-b",
        machine_type="e2-micro",
        condition_profile="timed-replay-v2",
        source_commit=COMMIT,
        repository_url="https://github.com/Daniele-Cangi/Aurora.git",
        evidence_dir=temp / "evidence",
    )


def write_campaign_sample(
    root: Path,
    index: int,
    *,
    commit: str = COMMIT,
    teardown: bool = True,
) -> None:
    sample = root / f"sample-{index:02d}"
    sample.mkdir(parents=True, exist_ok=True)
    value = {
        "schema": "aurora-raw-host-evidence-v2",
        "result": "passed",
        "project": "aurora-raw-test-12345",
        "source_commit": commit,
        "topology": "two-cross-region-non-peered-vpcs-public-ipv4",
        "runtime_binary_sha256": "a" * 64,
        "authentication_profile": "hmac-sha256-libsodium",
        "run_id": f"campaign-s{index:02d}",
        "teardown": {"success": teardown},
        "timing": {
            "sender_elapsed_ms": 390 + index * 10,
            "receiver_service_elapsed_ms": 1180 + index * 10,
            "controller_receiver_ready_ms": 2900 + index * 100,
            "controller_sender_wall_ms": 2800 + index * 100,
            "controller_total_wall_ms": 6500 + index * 100,
        },
    }
    (sample / "manifest.json").write_text(
        json.dumps(value), encoding="utf-8"
    )


def write_matrix_file(root: Path, *, samples: int = 1) -> Path:
    path = root / "matrix.json"
    value = {
        "schema": raw_matrix.MATRIX_SCHEMA,
        "matrix_id": "test-region-condition-matrix",
        "machine_type": "e2-micro",
        "cells": [
            {
                "id": "east-west-timed",
                "sender_zone": "us-east1-b",
                "receiver_zone": "us-west1-b",
                "condition_profile": "timed-replay-v2",
                "samples": samples,
            },
            {
                "id": "east-west-zero",
                "sender_zone": "us-east1-b",
                "receiver_zone": "us-west1-b",
                "condition_profile": "zero-delay-replay-v1",
                "samples": samples,
            },
            {
                "id": "east-europe-timed",
                "sender_zone": "us-east1-b",
                "receiver_zone": "europe-west1-b",
                "condition_profile": "timed-replay-v2",
                "samples": samples,
            },
            {
                "id": "east-europe-zero",
                "sender_zone": "us-east1-b",
                "receiver_zone": "europe-west1-b",
                "condition_profile": "zero-delay-replay-v1",
                "samples": samples,
            },
        ],
    }
    path.write_text(json.dumps(value), encoding="utf-8")
    return path


def matrix_context(root: Path) -> raw_matrix.RunContext:
    return raw_matrix.RunContext(
        project="aurora-raw-test-12345",
        run_prefix="test-matrix",
        source_commit=COMMIT,
        repository_url="https://github.com/Daniele-Cangi/Aurora.git",
        evidence_root=root / "evidence",
    )


def write_matrix_evidence(
    spec: raw_matrix.MatrixSpec, context: raw_matrix.RunContext
) -> None:
    runtime_hash = "a" * 64
    for index, run in enumerate(
        raw_matrix.planned_runs(spec, context), start=1
    ):
        run.evidence_dir.mkdir(parents=True, exist_ok=True)
        sender_elapsed = 400 + index
        receiver_elapsed = 1200 + index
        sender_text = (
            "sender_complete generations=2 protocol_version=2 "
            "replay_rejected=4 "
            "auth_profile=hmac-sha256-libsodium auth_rejected=0 "
            "feedback_rtt_samples=6 feedback_rtt_min_us=100 "
            "feedback_rtt_mean_us=200 feedback_rtt_max_us=300 "
            "terminal_feedback_rtt_samples=2 "
            "terminal_feedback_rtt_min_us=150 "
            "terminal_feedback_rtt_mean_us=250 "
            "terminal_feedback_rtt_max_us=350 "
            "unknown_feedback_echoes=0 feedback_applied=2 "
            f"sender_elapsed_ms={sender_elapsed}\n"
        )
        receiver_text = (
            "receiver_ready startup_timeout_ms=60000 "
            "service_timeout_ms=15000 protocol_version=2 "
            "deadline_semantics=descriptor-relative-receiver-steady "
            "regime_outage_generation_index=none "
            "auth_profile=hmac-sha256-libsodium\n"
            "receiver_complete generations=2 protocol_version=2 "
            "replay_rejected=5 "
            "auth_profile=hmac-sha256-libsodium auth_rejected=0 "
            f"service_elapsed_ms={receiver_elapsed}\n"
        )
        (run.evidence_dir / "sender.log").write_text(
            sender_text, encoding="utf-8"
        )
        (run.evidence_dir / "receiver.log").write_text(
            receiver_text, encoding="utf-8"
        )
        profile = harness.CONDITION_PROFILES[run.cell.condition_profile]
        value = {
            "schema": harness.RAW_EVIDENCE_SCHEMA,
            "evidence_level": "emulation",
            "result": "passed",
            "project": context.project,
            "run_id": run.run_id,
            "source_commit": context.source_commit,
            "topology": "two-cross-region-non-peered-vpcs-public-ipv4",
            "machine_type": spec.machine_type,
            "authentication_profile": "hmac-sha256-libsodium",
            "runtime_binary_sha256": runtime_hash,
            "condition": {
                "profile": profile.name,
                "forward_trace": profile.forward_name,
                "reverse_trace": profile.reverse_name,
            },
            "workload": {
                "id": "smoke-v2",
                "generation_count": 2,
                "startup_timeout_ms": harness.STARTUP_TIMEOUT_MS,
                "service_timeout_ms": harness.SERVICE_TIMEOUT_MS,
            },
            "measurement": harness.measurement_contract(),
            "sender": {
                "zone": run.cell.sender_zone,
                "binary_sha256": runtime_hash,
                "ntp_synchronized": True,
            },
            "receiver": {
                "zone": run.cell.receiver_zone,
                "binary_sha256": runtime_hash,
                "ntp_synchronized": True,
            },
            "logs": {
                "sender_sha256": hashlib.sha256(
                    sender_text.encode("utf-8")
                ).hexdigest(),
                "receiver_sha256": hashlib.sha256(
                    receiver_text.encode("utf-8")
                ).hexdigest(),
            },
            "timing": {
                "sender_elapsed_ms": sender_elapsed,
                "receiver_service_elapsed_ms": receiver_elapsed,
                "sender_replay_rejected": 4,
                "receiver_replay_rejected": 5,
                "feedback_rtt_samples": 6,
                "feedback_rtt_min_us": 100,
                "feedback_rtt_mean_us": 200,
                "feedback_rtt_max_us": 300,
                "terminal_feedback_rtt_samples": 2,
                "terminal_feedback_rtt_min_us": 150,
                "terminal_feedback_rtt_mean_us": 250,
                "terminal_feedback_rtt_max_us": 350,
                "unknown_feedback_echoes": 0,
                "controller_receiver_ready_ms": 2500 + index,
                "controller_sender_wall_ms": 3500 + index,
                "controller_total_wall_ms": 7000 + index,
            },
            "teardown": {"success": True, "remaining": {}},
            "claims": {
                "calibrated_performance": False,
                "field_evidence": False,
                "timing_scope": (
                    harness.TIMING_SCOPE
                ),
                "one_way_latency": False,
                "feedback_rtt_network_only": False,
            },
        }
        (run.evidence_dir / "manifest.json").write_text(
            json.dumps(value), encoding="utf-8"
        )


class FakeRunner:
    def __init__(self):
        self.commands = []

    def run(self, args, *, check=True, timeout=None):
        self.commands.append(list(args))
        is_list = args[1] == "compute" and "list" in args[2:6]
        return subprocess.CompletedProcess(
            args, 0 if is_list else 1, stdout="", stderr="not found"
        )


class GcpRawHostHarnessTests(unittest.TestCase):
    def test_windows_keygen_uses_gcloud_winkeygen_and_requires_ppk(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            sdk_bin = root / "google-cloud-sdk" / "bin"
            sdk_helpers = sdk_bin / "sdk"
            sdk_helpers.mkdir(parents=True)
            gcloud = sdk_bin / "gcloud.cmd"
            winkeygen = sdk_helpers / "winkeygen.exe"
            gcloud.touch()
            winkeygen.touch()
            key_file = root / "ephemeral-key"
            command = harness.ephemeral_ssh_keygen_command(
                str(gcloud),
                key_file,
                platform_name="win32",
                executable_lookup=lambda executable: (
                    str(gcloud) if executable == str(gcloud) else None
                ),
            )
            expected_files = harness.expected_ephemeral_ssh_key_files(
                key_file, platform_name="win32"
            )

        self.assertEqual(Path(command[0]), winkeygen.resolve())
        self.assertEqual(
            command[1:5],
            ["-bits", "3072", "-overwrite", str(key_file)],
        )
        self.assertEqual(
            expected_files,
            (key_file, Path(f"{key_file}.pub"), Path(f"{key_file}.ppk")),
        )

    def test_posix_keygen_keeps_openssh_command_and_key_pair(self):
        key_file = Path("ephemeral-key")
        command = harness.ephemeral_ssh_keygen_command(
            "gcloud",
            key_file,
            platform_name="linux",
            executable_lookup=lambda executable: (
                "/usr/bin/ssh-keygen" if executable == "ssh-keygen" else None
            ),
        )
        self.assertEqual(command[0], "/usr/bin/ssh-keygen")
        self.assertEqual(command[-2:], ["-f", str(key_file)])
        self.assertEqual(
            harness.expected_ephemeral_ssh_key_files(
                key_file, platform_name="linux"
            ),
            (key_file, Path(f"{key_file}.pub")),
        )

    def test_windows_keygen_fails_locally_when_winkeygen_is_missing(self):
        with tempfile.TemporaryDirectory() as name:
            gcloud = Path(name) / "gcloud.cmd"
            gcloud.touch()
            with self.assertRaisesRegex(harness.HarnessError, r"\.ppk"):
                harness.ephemeral_ssh_keygen_command(
                    str(gcloud),
                    Path(name) / "ephemeral-key",
                    platform_name="win32",
                    executable_lookup=lambda _executable: None,
                )

    def test_execute_prepares_ssh_key_before_provisioning(self):
        with tempfile.TemporaryDirectory() as name:
            cfg = config(Path(name))
            events = []

            class OrderingHarness(harness.GcpRawHarness):
                def prepare_ssh_key(self, _temp):
                    events.append("prepare-ssh-key")

                def provision(self):
                    events.append("provision")
                    return "192.0.2.1", "198.51.100.1"

                def prepare_runtime(self, _temp):
                    events.append("prepare-runtime")
                    raise harness.HarnessError("ordering sentinel")

                def cleanup(self):
                    events.append("cleanup")
                    return {"success": True, "remaining": {}}

            sut = OrderingHarness(cfg, FakeRunner(), gcloud="gcloud")
            with self.assertRaisesRegex(harness.HarnessError, "ordering sentinel"):
                sut.execute()

        self.assertEqual(
            events,
            ["prepare-ssh-key", "provision", "prepare-runtime", "cleanup"],
        )

    def test_plan_declares_non_peered_public_topology(self):
        with tempfile.TemporaryDirectory() as name:
            cfg = config(Path(name))
            cfg.validate()
            names = harness.Names.from_run_id(cfg.run_id)
            plan = harness.build_plan(cfg, names)
        self.assertEqual(
            plan["topology"],
            "two-cross-region-non-peered-vpcs-public-ipv4",
        )
        self.assertNotEqual(
            plan["sender"]["network"], plan["receiver"]["network"]
        )
        self.assertIn("/32", plan["firewall"]["forward"])
        self.assertIn("automatic", plan["safety"]["vm_max_run_duration"])
        self.assertFalse(plan["safety"]["project_deletion"])
        self.assertEqual(
            plan["treatment"]["policy_id"], "biological-adaptive"
        )
        self.assertEqual(plan["workload"]["id"], "smoke-v2")

    def test_policy_pilot_is_frozen_bounded_and_plan_only(self):
        spec = policy_pilot.Spec.load(
            ROOT / "benchmarks" / "gcp_raw_host_policy_pilot_v1.json"
        )
        with tempfile.TemporaryDirectory() as name:
            context = policy_pilot.Context(
                project="aurora-raw-test-12345",
                run_prefix="policy-test",
                source_commit=COMMIT,
                repository_url="https://github.com/Daniele-Cangi/Aurora.git",
                evidence_root=Path(name) / "evidence",
            )
            plan = policy_pilot.build_plan(spec, context)
            runs = policy_pilot.planned_runs(spec, context)

        self.assertEqual(plan["lifecycle_count"], 12)
        self.assertTrue(plan["safety"]["fresh_vm_pair_per_lifecycle"])
        self.assertTrue(
            plan["safety"]["reviewed_source_commit_required_for_execute"]
        )
        self.assertFalse(plan["claims"]["confirmatory"])
        self.assertEqual(len({run.run_id for run in runs}), 12)
        for start in (0, 6):
            block = runs[start:start + 6]
            self.assertEqual({run.cell.id for run in block}, {
                cell.id for cell in spec.cells
            })
        self.assertEqual(
            {run.cell.policy for run in runs}, set(harness.POLICY_IDS)
        )
        self.assertEqual(
            {run.cell.condition for run in runs},
            {"timed-replay-v2", "regime-change-v1"},
        )
        for run in runs:
            cfg = policy_pilot.run_config(spec, context, run)
            self.assertEqual(cfg.workload_id, "policy-pilot-v1")
            self.assertEqual(cfg.policy_id, run.cell.policy)
            expected_outage = 2 if run.cell.condition == "regime-change-v1" \
                else None
            profile = harness.CONDITION_PROFILES[cfg.condition_profile]
            self.assertEqual(
                profile.outage_generation_index,
                expected_outage,
            )

    def test_unsafe_run_id_is_rejected(self):
        with tempfile.TemporaryDirectory() as name:
            cfg = config(Path(name), run_id="../unsafe")
            with self.assertRaises(harness.HarnessError):
                cfg.validate()

    def test_process_evidence_requires_timing_and_authentication(self):
        sender = (
            "sender_complete generations=2 protocol_version=2 "
            "replay_rejected=4 "
            "auth_profile=hmac-sha256-libsodium auth_rejected=0 "
            "feedback_applied=2 feedback_rtt_samples=6 "
            "feedback_rtt_min_us=100 feedback_rtt_mean_us=200 "
            "feedback_rtt_max_us=300 terminal_feedback_rtt_samples=2 "
            "terminal_feedback_rtt_min_us=150 "
            "terminal_feedback_rtt_mean_us=250 "
            "terminal_feedback_rtt_max_us=350 "
            "unknown_feedback_echoes=0 "
            "sender_elapsed_ms=1234\n"
        )
        receiver = (
            "receiver_ready startup_timeout_ms=60000 "
            "service_timeout_ms=15000 protocol_version=2 "
            "deadline_semantics=descriptor-relative-receiver-steady "
            "regime_outage_generation_index=none "
            "auth_profile=hmac-sha256-libsodium\n"
            "receiver_complete generations=2 protocol_version=2 "
            "replay_rejected=5 "
            "auth_profile=hmac-sha256-libsodium auth_rejected=0 "
            "service_elapsed_ms=987\n"
        )
        result = harness.verify_process_logs(sender, receiver)
        self.assertEqual(result["sender_elapsed_ms"], 1234)
        self.assertEqual(result["receiver_service_elapsed_ms"], 987)
        self.assertEqual(result["feedback_rtt_samples"], 6)
        self.assertEqual(result["terminal_feedback_rtt_samples"], 2)
        self.assertEqual(result["terminal_feedback_rtt_mean_us"], 250)

    def test_instance_has_billing_and_credential_safety_guards(self):
        with tempfile.TemporaryDirectory() as name:
            cfg = config(Path(name))
            fake = FakeRunner()
            sut = harness.GcpRawHarness(cfg, fake, gcloud="gcloud")
            sut._create_instance(
                sut.names.sender_instance,
                cfg.sender_zone,
                sut.names.sender_subnet,
                "aurora-iap,aurora-tx",
            )
        command = fake.commands[0]
        self.assertIn("--max-run-duration=30m", command)
        self.assertIn("--instance-termination-action=DELETE", command)
        self.assertIn("--no-service-account", command)
        self.assertIn("--no-scopes", command)

    def test_ssh_and_scp_require_the_explicit_ephemeral_key(self):
        with tempfile.TemporaryDirectory() as name:
            cfg = config(Path(name))
            fake = FakeRunner()
            sut = harness.GcpRawHarness(cfg, fake, gcloud="gcloud")
            with self.assertRaises(harness.HarnessError):
                sut.ssh_args(sut.names.sender_instance, cfg.sender_zone, "true")
            sut.ssh_key_file = Path(name) / "ephemeral-ssh-key"
            ssh = sut.ssh_args(
                sut.names.sender_instance, cfg.sender_zone, "true"
            )
            sut.scp(
                "source",
                f"{sut.names.sender_instance}:/tmp/destination",
                zone=cfg.sender_zone,
            )
        expected = f"--ssh-key-file={sut.ssh_key_file}"
        self.assertIn(expected, ssh)
        self.assertIn(expected, fake.commands[0])

    def test_cleanup_is_exact_and_idempotent(self):
        with tempfile.TemporaryDirectory() as name:
            cfg = config(Path(name))
            fake = FakeRunner()
            sut = harness.GcpRawHarness(cfg, fake, gcloud="gcloud")
            result = sut.cleanup()
            names = harness.Names.from_run_id(cfg.run_id)

        self.assertTrue(result["success"])
        flattened = json.dumps(fake.commands)
        self.assertNotIn("projects delete", flattened)
        self.assertNotIn("*", flattened)
        self.assertIn(names.sender_instance, flattened)
        self.assertIn(names.receiver_instance, flattened)
        self.assertIn("--delete-disks=all", flattened)
        self.assertIn(names.sender_network, flattened)
        self.assertIn(names.receiver_network, flattened)
        self.assertEqual(
            set(result["remaining"]),
            {
                "instances",
                "disks",
                "addresses",
                "snapshots",
                "firewall_rules",
                "subnets",
                "networks",
            },
        )
        first = fake.commands[0]
        self.assertEqual(first[1:4], ["compute", "instances", "delete"])
        for command in fake.commands:
            self.assertIn(f"--project={cfg.project}", command)

    def test_campaign_summary_requires_common_identity_and_teardown(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            for index in range(1, 4):
                write_campaign_sample(root, index)
            result = campaign.summarize(root, 3)
            self.assertEqual(result["samples_observed"], 3)
            self.assertEqual(
                result["timing"]["sender_elapsed_ms"],
                {
                    "count": 3,
                    "min": 400.0,
                    "max": 420.0,
                    "mean": 410.0,
                    "median": 410.0,
                    "sample_stddev": 10.0,
                },
            )

            write_campaign_sample(root, 2, commit="f" * 40)
            with self.assertRaises(campaign.CampaignError):
                campaign.summarize(root, 3)

            write_campaign_sample(root, 2, teardown=False)
            with self.assertRaises(campaign.CampaignError):
                campaign.summarize(root, 3)

    def test_matrix_plan_is_balanced_bounded_and_exact(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            spec = raw_matrix.MatrixSpec.load(write_matrix_file(root))
            context = matrix_context(root)
            result = raw_matrix.build_plan(spec, context)
        self.assertEqual(result["samples_total"], 4)
        self.assertTrue(result["safety"]["balanced_complete_factorial"])
        self.assertEqual(len(set(result["run_ids"])), 4)
        self.assertTrue(all(len(value) <= 31 for value in result["run_ids"]))

    def test_measurement_v2_contract_and_randomized_pilot_are_predeclared(self):
        contract = json.loads(
            (ROOT / "benchmarks" / "process_measurement_contract_v2.json")
            .read_text(encoding="utf-8")
        )
        self.assertEqual(contract["profile"], harness.MEASUREMENT_PROFILE)
        self.assertEqual(
            contract["process_protocol_version"],
            harness.PROCESS_PROTOCOL_VERSION,
        )
        self.assertTrue(contract["prohibitions"]["cross_clock_subtraction"])
        self.assertTrue(contract["prohibitions"]["one_way_latency_claim"])

        pilot_path = (
            ROOT / "benchmarks" / "gcp_raw_measurement_pilot_v2.json"
        )
        pilot_value = json.loads(pilot_path.read_text(encoding="utf-8"))
        spec = raw_matrix.MatrixSpec.load(pilot_path)
        with tempfile.TemporaryDirectory() as name:
            result = raw_matrix.build_plan(spec, matrix_context(Path(name)))
            tampered = dict(pilot_value)
            tampered["execution_order"] = list(pilot_value["execution_order"])
            tampered["execution_order"][0], tampered["execution_order"][1] = (
                tampered["execution_order"][1],
                tampered["execution_order"][0],
            )
            invalid_path = Path(name) / "tampered-pilot.json"
            invalid_path.write_text(json.dumps(tampered), encoding="utf-8")
            with self.assertRaises(raw_matrix.MatrixError):
                raw_matrix.MatrixSpec.load(invalid_path)

        self.assertEqual(result["samples_total"], 12)
        self.assertEqual(
            result["measurement_profile"], harness.MEASUREMENT_PROFILE
        )
        self.assertEqual(
            result["execution_cell_order"], pilot_value["execution_order"]
        )
        self.assertEqual(
            result["randomization"]["method"],
            "randomized-complete-blocks-sha256-v1",
        )
        for start in range(0, 12, 4):
            self.assertEqual(
                set(result["execution_cell_order"][start:start + 4]),
                {cell.id for cell in spec.cells},
            )
        self.assertFalse(result["claims"]["causal_region_effect"])
        self.assertFalse(result["claims"]["causal_condition_effect"])

    def test_powered_condition_study_is_preregistered_and_bounded(self):
        study_path = (
            ROOT / "benchmarks" / "gcp_raw_powered_condition_study_v3.json"
        )
        study = json.loads(study_path.read_text(encoding="utf-8"))
        report = power_analysis.evaluate_study(study_path)

        self.assertEqual(
            report["pilot"]["block_contrasts_us"],
            [58369.0, 57594.0, 58140.0],
        )
        self.assertEqual(report["design"]["required_blocks"], 8)
        self.assertEqual(report["design"]["total_lifecycles"], 32)
        self.assertLess(
            report["simulations"][1]["one_sided_95_percent_wilson_lower"],
            study["power"]["target_power"],
        )
        self.assertGreaterEqual(
            report["simulations"][2]["one_sided_95_percent_wilson_lower"],
            study["power"]["target_power"],
        )
        self.assertFalse(study["analysis"]["interim_analysis"])
        self.assertFalse(study["analysis"]["early_stopping"])
        self.assertFalse(study["exclusion_and_stopping"]["replacement_blocks"])
        self.assertEqual(study["schedule"]["minimum_distinct_utc_dates"], 4)
        self.assertEqual(study["safety"]["maximum_lifecycles_per_dispatch"], 8)
        self.assertTrue(study["claims"]["causal_region_effect"] is False)

        total_samples = 0
        for campaign in study["campaigns"]:
            matrix_path = ROOT / campaign["matrix"]
            spec = raw_matrix.MatrixSpec.load(matrix_path)
            with tempfile.TemporaryDirectory() as name:
                plan = raw_matrix.build_plan(spec, matrix_context(Path(name)))
            self.assertEqual(plan["samples_total"], 8)
            self.assertEqual(campaign["blocks"], 2)
            self.assertEqual(campaign["lifecycles"], 8)
            for start in (0, 4):
                self.assertEqual(
                    set(plan["execution_cell_order"][start:start + 4]),
                    {cell.id for cell in spec.cells},
                )
            total_samples += plan["samples_total"]
        self.assertEqual(total_samples, 32)

        workflow = (
            ROOT / ".github" / "workflows" / "gcp-raw-host-matrix.yml"
        ).read_text(encoding="utf-8")
        for campaign in study["campaigns"]:
            self.assertIn(campaign["id"], workflow)
            self.assertIn(campaign["matrix"], workflow)
        self.assertIn("CREATE_AND_DELETE", workflow)
        self.assertIn('test "${AURORA_STUDY_COMMIT}" = "${GITHUB_SHA}"', workflow)
        self.assertNotIn("gcp_raw_measurement_pilot_v2.json", workflow)

    def test_matrix_rejects_incomplete_or_unbalanced_factorial(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            path = write_matrix_file(root)
            value = json.loads(path.read_text(encoding="utf-8"))
            value["cells"].pop()
            path.write_text(json.dumps(value), encoding="utf-8")
            with self.assertRaises(raw_matrix.MatrixError):
                raw_matrix.MatrixSpec.load(path)

            path = write_matrix_file(root)
            value = json.loads(path.read_text(encoding="utf-8"))
            value["cells"][0]["samples"] = 2
            path.write_text(json.dumps(value), encoding="utf-8")
            with self.assertRaises(raw_matrix.MatrixError):
                raw_matrix.MatrixSpec.load(path)

    def test_matrix_summary_binds_factors_logs_and_measurement(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            spec = raw_matrix.MatrixSpec.load(write_matrix_file(root))
            context = matrix_context(root)
            write_matrix_evidence(spec, context)
            result = raw_matrix.summarize(spec, context)
            self.assertEqual(result["cells_observed"], 4)
            self.assertEqual(result["samples_observed"], 4)
            self.assertEqual(
                result["overall_timing"]["sender_elapsed_ms"]["count"], 4
            )
            self.assertFalse(result["claims"]["calibrated_performance"])
            self.assertFalse(result["claims"]["causal_region_effect"])

            first = raw_matrix.planned_runs(spec, context)[0]
            path = first.evidence_dir / "manifest.json"
            value = json.loads(path.read_text(encoding="utf-8"))
            value["condition"]["profile"] = "wrong"
            path.write_text(json.dumps(value), encoding="utf-8")
            with self.assertRaises(raw_matrix.MatrixError):
                raw_matrix.summarize(spec, context)

    def test_matrix_cleanup_visits_every_exact_run(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            spec = raw_matrix.MatrixSpec.load(write_matrix_file(root))
            context = matrix_context(root)
            fake = FakeRunner()
            result = raw_matrix.cleanup_matrix(
                spec, context, fake, gcloud="gcloud"
            )
            flattened = json.dumps(fake.commands)
        self.assertTrue(result["success"])
        self.assertEqual(result["runs_observed"], 4)
        for run in raw_matrix.planned_runs(spec, context):
            self.assertIn(run.run_id, flattened)
        self.assertNotIn("*", flattened)
        self.assertNotIn("projects delete", flattened)


if __name__ == "__main__":
    unittest.main()
