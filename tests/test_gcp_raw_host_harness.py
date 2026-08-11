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
            "sender_complete generations=2 replay_rejected=4 "
            "auth_profile=hmac-sha256-libsodium auth_rejected=0 "
            f"feedback_applied=2 sender_elapsed_ms={sender_elapsed}\n"
        )
        receiver_text = (
            "receiver_ready startup_timeout_ms=60000 "
            "service_timeout_ms=15000 "
            "auth_profile=hmac-sha256-libsodium\n"
            "receiver_complete generations=2 replay_rejected=5 "
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
            "schema": "aurora-raw-host-evidence-v2",
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
                "generation_count": 2,
                "startup_timeout_ms": harness.STARTUP_TIMEOUT_MS,
                "service_timeout_ms": harness.SERVICE_TIMEOUT_MS,
            },
            "measurement": {
                "profile": harness.MEASUREMENT_PROFILE,
                "clock_relationship": (
                    "independent-unsynchronized-steady-clocks"
                ),
                "provisioning_included": False,
                "teardown_included": False,
            },
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
                "controller_receiver_ready_ms": 2500 + index,
                "controller_sender_wall_ms": 3500 + index,
                "controller_total_wall_ms": 7000 + index,
            },
            "teardown": {"success": True, "remaining": {}},
            "claims": {
                "calibrated_performance": False,
                "field_evidence": False,
                "timing_scope": (
                    "application-and-controller-steady-clock"
                ),
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

    def test_unsafe_run_id_is_rejected(self):
        with tempfile.TemporaryDirectory() as name:
            cfg = config(Path(name), run_id="../unsafe")
            with self.assertRaises(harness.HarnessError):
                cfg.validate()

    def test_process_evidence_requires_timing_and_authentication(self):
        sender = (
            "sender_complete generations=2 replay_rejected=4 "
            "auth_profile=hmac-sha256-libsodium auth_rejected=0 "
            "feedback_applied=2 "
            "sender_elapsed_ms=1234\n"
        )
        receiver = (
            "receiver_ready startup_timeout_ms=60000 "
            "service_timeout_ms=15000 "
            "auth_profile=hmac-sha256-libsodium\n"
            "receiver_complete generations=2 replay_rejected=5 "
            "auth_profile=hmac-sha256-libsodium auth_rejected=0 "
            "service_elapsed_ms=987\n"
        )
        result = harness.verify_process_logs(sender, receiver)
        self.assertEqual(result["sender_elapsed_ms"], 1234)
        self.assertEqual(result["receiver_service_elapsed_ms"], 987)

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
