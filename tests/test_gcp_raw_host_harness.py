import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import gcp_raw_host_emulation as harness  # noqa: E402
import summarize_gcp_raw_campaign as campaign  # noqa: E402


COMMIT = "0123456789abcdef0123456789abcdef01234567"


def config(temp: Path, run_id="test-run-001"):
    return harness.Config(
        project="aurora-raw-test-12345",
        run_id=run_id,
        sender_zone="us-east1-b",
        receiver_zone="us-west1-b",
        machine_type="e2-micro",
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
            "auth_profile=hmac-sha256-libsodium feedback_applied=2 "
            "sender_elapsed_ms=1234\n"
        )
        receiver = (
            "receiver_ready startup_timeout_ms=60000 "
            "service_timeout_ms=15000 "
            "auth_profile=hmac-sha256-libsodium\n"
            "receiver_complete generations=2 replay_rejected=5 "
            "auth_profile=hmac-sha256-libsodium service_elapsed_ms=987\n"
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


if __name__ == "__main__":
    unittest.main()
