#!/usr/bin/env python3
"""Provision, run, verify, and tear down Aurora raw-routed GCP evidence.

The default mode is read-only and prints a plan. Resource creation requires
both --execute and --acknowledge-billing-and-teardown. Cleanup is exact-name,
idempotent, and runs both from the main finally block and from the workflow's
independent always() cleanup step.
"""

from __future__ import annotations

import argparse
import dataclasses
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor
from typing import Iterable


FORWARD_PORT = 47001
REVERSE_PORT = 47002
STARTUP_TIMEOUT_MS = 60_000
SERVICE_TIMEOUT_MS = 15_000
IAP_SOURCE_CIDR = "35.235.240.0/20"
NAME_PATTERN = re.compile(r"^[a-z][a-z0-9-]{2,30}$")
PROJECT_PATTERN = re.compile(r"^[a-z][a-z0-9-]{4,28}[a-z0-9]$")
COMMIT_PATTERN = re.compile(r"^[0-9a-f]{40}$")
RAW_EVIDENCE_SCHEMA = "aurora-raw-host-evidence-v3"
PROCESS_PROTOCOL_VERSION = 2
MEASUREMENT_PROFILE = "application-controller-steady-v2"
TIMING_SCOPE = "application-controller-and-sender-feedback-steady-clocks"
FEEDBACK_RTT_TIMING_FIELDS = (
    "feedback_rtt_min_us",
    "feedback_rtt_mean_us",
    "feedback_rtt_max_us",
    "terminal_feedback_rtt_min_us",
    "terminal_feedback_rtt_mean_us",
    "terminal_feedback_rtt_max_us",
)
POLICY_IDS = (
    "fixed-minimum",
    "fixed-class-aware",
    "biological-adaptive",
)
WORKLOAD_GENERATIONS = {
    "smoke-v2": 2,
    "policy-pilot-v1": 8,
}


def workload_service_timeout_ms(workload_id: str) -> int:
    return 120_000 if workload_id == "policy-pilot-v1" else SERVICE_TIMEOUT_MS


def measurement_contract() -> dict:
    return {
        "profile": MEASUREMENT_PROFILE,
        "process_protocol_version": PROCESS_PROTOCOL_VERSION,
        "clock_relationship": "independent-unsynchronized-steady-clocks",
        "same_clock_metrics": {
            "sender_steady": [
                "sender_elapsed_ms",
                *FEEDBACK_RTT_TIMING_FIELDS,
            ],
            "receiver_steady": ["receiver_service_elapsed_ms"],
            "controller_steady": [
                "controller_receiver_ready_ms",
                "controller_sender_wall_ms",
                "controller_total_wall_ms",
            ],
        },
        "cross_clock_subtraction_permitted": False,
        "provisioning_included": False,
        "teardown_included": False,
    }


@dataclasses.dataclass(frozen=True)
class ConditionProfile:
    name: str
    forward_trace: str
    reverse_trace: str

    @property
    def forward_name(self) -> str:
        return self.forward_trace.rsplit("/", 1)[-1]

    @property
    def reverse_name(self) -> str:
        return self.reverse_trace.rsplit("/", 1)[-1]


CONDITION_PROFILES = {
    "timed-replay-v2": ConditionProfile(
        name="timed-replay-v2",
        forward_trace="benchmarks/process_timed_v2.trace",
        reverse_trace="benchmarks/process_feedback_v2.trace",
    ),
    "zero-delay-replay-v1": ConditionProfile(
        name="zero-delay-replay-v1",
        forward_trace="benchmarks/process_zero_delay_forward_v1.trace",
        reverse_trace="benchmarks/process_zero_delay_reverse_v1.trace",
    ),
    "feedback-stall-v2": ConditionProfile(
        name="feedback-stall-v2",
        forward_trace="benchmarks/process_zero_delay_forward_v1.trace",
        reverse_trace="benchmarks/process_feedback_v2.trace",
    ),
}


class HarnessError(RuntimeError):
    pass


class CommandRunner:
    def __init__(self, *, verbose: bool = True):
        self.verbose = verbose

    def run(
        self,
        args: list[str],
        *,
        check: bool = True,
        timeout: int | None = None,
    ) -> subprocess.CompletedProcess[str]:
        if self.verbose:
            print("+ " + " ".join(args), flush=True)
        result = subprocess.run(
            args,
            capture_output=True,
            text=True,
            timeout=timeout,
            check=False,
        )
        if check and result.returncode != 0:
            detail = (result.stderr or result.stdout or "").strip()[-3000:]
            raise HarnessError(
                f"command failed ({result.returncode}): {args[0]}: {detail}"
            )
        return result

    def popen(
        self,
        args: list[str],
        *,
        stdout,
        stderr,
    ) -> subprocess.Popen[str]:
        if self.verbose:
            print("+ " + " ".join(args), flush=True)
        return subprocess.Popen(
            args,
            stdout=stdout,
            stderr=stderr,
            text=True,
        )


@dataclasses.dataclass(frozen=True)
class Config:
    project: str
    run_id: str
    sender_zone: str
    receiver_zone: str
    machine_type: str
    condition_profile: str
    source_commit: str
    repository_url: str
    evidence_dir: Path
    policy_id: str = "biological-adaptive"
    workload_id: str = "smoke-v2"

    def validate(self) -> None:
        if not PROJECT_PATTERN.fullmatch(self.project):
            raise HarnessError("invalid GCP project id")
        if not NAME_PATTERN.fullmatch(self.run_id):
            raise HarnessError(
                "run id must be 3..31 lowercase letters, digits, or hyphens"
            )
        if not COMMIT_PATTERN.fullmatch(self.source_commit):
            raise HarnessError("source commit must be a full lowercase SHA-1")
        for zone in (self.sender_zone, self.receiver_zone):
            if not re.fullmatch(r"[a-z]+-[a-z]+[0-9]-[a-z]", zone):
                raise HarnessError(f"invalid GCP zone: {zone}")
        if self.sender_zone == self.receiver_zone:
            raise HarnessError("sender and receiver zones must differ")
        if not re.fullmatch(r"[a-z][a-z0-9-]{1,30}", self.machine_type):
            raise HarnessError("invalid machine type")
        if self.condition_profile not in CONDITION_PROFILES:
            raise HarnessError("unknown condition profile")
        if self.policy_id not in POLICY_IDS:
            raise HarnessError("unknown transport policy")
        if self.workload_id not in WORKLOAD_GENERATIONS:
            raise HarnessError("unknown process workload")
        if not self.repository_url.startswith("https://github.com/"):
            raise HarnessError("repository URL must use GitHub HTTPS")


@dataclasses.dataclass(frozen=True)
class Names:
    prefix: str
    sender_network: str
    receiver_network: str
    sender_subnet: str
    receiver_subnet: str
    sender_instance: str
    receiver_instance: str
    sender_iap_firewall: str
    receiver_iap_firewall: str
    forward_firewall: str
    reverse_firewall: str

    @staticmethod
    def from_run_id(run_id: str) -> "Names":
        prefix = f"aurora-{run_id}"
        if len(prefix) > 38:
            raise HarnessError("run id produces resource names that are too long")
        return Names(
            prefix=prefix,
            sender_network=f"{prefix}-tx-net",
            receiver_network=f"{prefix}-rx-net",
            sender_subnet=f"{prefix}-tx-subnet",
            receiver_subnet=f"{prefix}-rx-subnet",
            sender_instance=f"{prefix}-tx",
            receiver_instance=f"{prefix}-rx",
            sender_iap_firewall=f"{prefix}-tx-iap",
            receiver_iap_firewall=f"{prefix}-rx-iap",
            forward_firewall=f"{prefix}-forward",
            reverse_firewall=f"{prefix}-reverse",
        )


def zone_region(zone: str) -> str:
    return zone.rsplit("-", 1)[0]


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def parse_record(text: str, prefix: str) -> dict[str, str]:
    for line in text.splitlines():
        if line.startswith(prefix + " "):
            fields: dict[str, str] = {}
            for token in line.split()[1:]:
                if "=" not in token:
                    continue
                key, value = token.split("=", 1)
                fields[key] = value
            return fields
    raise HarnessError(f"missing {prefix} record")


def require_positive(fields: dict[str, str], key: str) -> int:
    try:
        value = int(fields[key])
    except (KeyError, ValueError) as error:
        raise HarnessError(f"invalid or missing {key}") from error
    if value <= 0:
        raise HarnessError(f"{key} must be positive")
    return value


def require_nonnegative(fields: dict[str, str], key: str) -> int:
    try:
        value = int(fields[key])
    except (KeyError, ValueError) as error:
        raise HarnessError(f"invalid or missing {key}") from error
    if value < 0:
        raise HarnessError(f"{key} must be nonnegative")
    return value


def verify_process_logs(
    sender_text: str,
    receiver_text: str,
    *,
    expected_generations: int = 2,
    expected_policy_id: str | None = None,
    expected_workload_id: str | None = None,
    expected_service_timeout_ms: int = SERVICE_TIMEOUT_MS,
) -> dict:
    ready = parse_record(receiver_text, "receiver_ready")
    sender = parse_record(sender_text, "sender_complete")
    receiver = parse_record(receiver_text, "receiver_complete")
    if ready.get("startup_timeout_ms") != str(STARTUP_TIMEOUT_MS):
        raise HarnessError("receiver startup timeout evidence mismatch")
    if ready.get("service_timeout_ms") != str(expected_service_timeout_ms):
        raise HarnessError("receiver service timeout evidence mismatch")
    if ready.get("auth_profile") != "hmac-sha256-libsodium":
        raise HarnessError("receiver readiness is not authenticated")
    for role, fields in (("sender", sender), ("receiver", receiver)):
        if fields.get("generations") != str(expected_generations):
            raise HarnessError(
                f"{role} did not complete {expected_generations} generations"
            )
        if fields.get("auth_profile") != "hmac-sha256-libsodium":
            raise HarnessError(f"{role} did not use libsodium HMAC")
        if fields.get("auth_rejected") != "0":
            raise HarnessError(f"{role} rejected authenticated evidence")
        if fields.get("protocol_version") != str(PROCESS_PROTOCOL_VERSION):
            raise HarnessError(f"{role} process protocol version mismatch")
        require_positive(fields, "replay_rejected")
    if sender.get("feedback_applied") != str(expected_generations):
        raise HarnessError("sender did not apply every terminal feedback report")
    if expected_policy_id is not None and \
            sender.get("policy_id") != expected_policy_id:
        raise HarnessError("sender policy treatment mismatch")
    if expected_workload_id is not None and \
            sender.get("workload_id") != expected_workload_id:
        raise HarnessError("sender workload mismatch")
    if ready.get("protocol_version") != str(PROCESS_PROTOCOL_VERSION):
        raise HarnessError("receiver readiness protocol version mismatch")
    feedback_samples = require_positive(sender, "feedback_rtt_samples")
    terminal_samples = require_positive(
        sender, "terminal_feedback_rtt_samples"
    )
    if terminal_samples != expected_generations or \
            feedback_samples < terminal_samples:
        raise HarnessError("sender feedback RTT sample counts are inconsistent")
    if require_nonnegative(sender, "unknown_feedback_echoes") != 0:
        raise HarnessError("sender observed an unbound feedback echo")
    rtt_values = {
        field: require_positive(sender, field)
        for field in FEEDBACK_RTT_TIMING_FIELDS
    }
    for prefix in ("feedback_rtt", "terminal_feedback_rtt"):
        if not (
            rtt_values[f"{prefix}_min_us"]
            <= rtt_values[f"{prefix}_mean_us"]
            <= rtt_values[f"{prefix}_max_us"]
        ):
            raise HarnessError(f"invalid {prefix} summary")
    result = {
        "sender_elapsed_ms": require_positive(sender, "sender_elapsed_ms"),
        "receiver_service_elapsed_ms": require_positive(
            receiver, "service_elapsed_ms"
        ),
        "sender_replay_rejected": int(sender["replay_rejected"]),
        "receiver_replay_rejected": int(receiver["replay_rejected"]),
        "feedback_rtt_samples": feedback_samples,
        **rtt_values,
        "terminal_feedback_rtt_samples": terminal_samples,
        "unknown_feedback_echoes": 0,
    }
    pilot_fields = (
        "delivered",
        "critical_generations",
        "critical_delivered_before_deadline",
        "source_symbols",
        "initial_symbols",
        "repair_symbols_requested",
        "repair_symbols_emitted",
        "descriptor_datagrams",
        "wire_symbol_datagrams",
    )
    if expected_workload_id == "policy-pilot-v1":
        for field in pilot_fields:
            result[field] = require_nonnegative(sender, field)
        if result["source_symbols"] <= 0 or result["initial_symbols"] <= 0:
            raise HarnessError("pilot symbol counters must be positive")
        receiver_delivered = require_nonnegative(receiver, "delivered")
        receiver_critical = require_nonnegative(
            receiver, "critical_generations"
        )
        receiver_critical_before = require_nonnegative(
            receiver, "critical_delivered_before_deadline"
        )
        if receiver_delivered != result["delivered"] or \
                receiver_critical != result["critical_generations"] or \
                receiver_critical_before != \
                result["critical_delivered_before_deadline"]:
            raise HarnessError("sender/receiver delivery outcomes disagree")
        if result["critical_generations"] != expected_generations:
            raise HarnessError("pilot workload lacks a critical segment")
    return result


def build_plan(config: Config, names: Names) -> dict:
    condition = CONDITION_PROFILES[config.condition_profile]
    return {
        "schema": "aurora-gcp-raw-plan-v2",
        "project": config.project,
        "run_id": config.run_id,
        "source_commit": config.source_commit,
        "topology": "two-cross-region-non-peered-vpcs-public-ipv4",
        "machine_type": config.machine_type,
        "treatment": {
            "policy_id": config.policy_id,
            "selection": "sender-runtime-argument",
            "binary_rebuild_per_treatment": False,
        },
        "workload": {
            "id": config.workload_id,
            "generation_count": WORKLOAD_GENERATIONS[config.workload_id],
            "startup_timeout_ms": STARTUP_TIMEOUT_MS,
            "service_timeout_ms": workload_service_timeout_ms(
                config.workload_id
            ),
        },
        "condition": {
            "profile": condition.name,
            "forward_trace": condition.forward_name,
            "reverse_trace": condition.reverse_name,
        },
        "measurement": measurement_contract(),
        "sender": {
            "zone": config.sender_zone,
            "region": zone_region(config.sender_zone),
            "network": names.sender_network,
            "subnet": names.sender_subnet,
            "instance": names.sender_instance,
            "cidr": "10.44.1.0/24",
        },
        "receiver": {
            "zone": config.receiver_zone,
            "region": zone_region(config.receiver_zone),
            "network": names.receiver_network,
            "subnet": names.receiver_subnet,
            "instance": names.receiver_instance,
            "cidr": "10.45.1.0/24",
        },
        "firewall": {
            "ssh": f"tcp:22 from {IAP_SOURCE_CIDR}",
            "forward": f"udp:{FORWARD_PORT} from sender public /32",
            "reverse": f"udp:{REVERSE_PORT} from receiver public /32",
        },
        "safety": {
            "default_mode": "plan-only",
            "execute_requires_acknowledgement": True,
            "vm_max_run_duration": "30m; automatic instance deletion",
            "cleanup": "exact names; instances and disks first; no wildcards",
            "project_deletion": False,
        },
        "claims": {
            "calibrated_performance": False,
            "field_evidence": False,
            "one_way_latency": False,
            "feedback_rtt_network_only": False,
            "causal_region_effect": False,
            "causal_condition_effect": False,
        },
    }


class GcpRawHarness:
    def __init__(
        self,
        config: Config,
        runner: CommandRunner,
        *,
        gcloud: str,
    ):
        config.validate()
        self.config = config
        self.runner = runner
        self.gcloud = gcloud
        self.names = Names.from_run_id(config.run_id)
        self.ssh_key_file: Path | None = None

    def gc(self, *args: str, check: bool = True, timeout: int | None = None):
        command = [self.gcloud, *args, f"--project={self.config.project}"]
        return self.runner.run(command, check=check, timeout=timeout)

    def ssh_args(self, instance: str, zone: str, command: str) -> list[str]:
        if self.ssh_key_file is None:
            raise HarnessError("ephemeral SSH key has not been prepared")
        return [
            self.gcloud,
            "compute",
            "ssh",
            instance,
            f"--project={self.config.project}",
            f"--zone={zone}",
            "--tunnel-through-iap",
            "--quiet",
            f"--ssh-key-file={self.ssh_key_file}",
            f"--command={command}",
        ]

    def ssh(self, instance: str, zone: str, command: str, *, timeout=600):
        return self.runner.run(
            self.ssh_args(instance, zone, command), timeout=timeout
        )

    def scp(
        self,
        source: str,
        destination: str,
        *,
        zone: str,
        timeout: int = 180,
    ) -> None:
        if self.ssh_key_file is None:
            raise HarnessError("ephemeral SSH key has not been prepared")
        self.runner.run(
            [
                self.gcloud,
                "compute",
                "scp",
                f"--project={self.config.project}",
                f"--zone={zone}",
                "--tunnel-through-iap",
                "--quiet",
                f"--ssh-key-file={self.ssh_key_file}",
                source,
                destination,
            ],
            timeout=timeout,
        )

    def provision(self) -> tuple[str, str]:
        n = self.names
        sender_region = zone_region(self.config.sender_zone)
        receiver_region = zone_region(self.config.receiver_zone)
        for network in (n.sender_network, n.receiver_network):
            self.gc(
                "compute", "networks", "create", network,
                "--subnet-mode=custom", timeout=120,
            )
        self.gc(
            "compute", "networks", "subnets", "create", n.sender_subnet,
            f"--network={n.sender_network}", f"--region={sender_region}",
            "--range=10.44.1.0/24", timeout=120,
        )
        self.gc(
            "compute", "networks", "subnets", "create", n.receiver_subnet,
            f"--network={n.receiver_network}", f"--region={receiver_region}",
            "--range=10.45.1.0/24", timeout=120,
        )
        for firewall, network in (
            (n.sender_iap_firewall, n.sender_network),
            (n.receiver_iap_firewall, n.receiver_network),
        ):
            self.gc(
                "compute", "firewall-rules", "create", firewall,
                f"--network={network}", "--direction=INGRESS",
                "--action=ALLOW", "--rules=tcp:22",
                f"--source-ranges={IAP_SOURCE_CIDR}",
                "--target-tags=aurora-iap", timeout=120,
            )
        self._create_instance(
            n.sender_instance, self.config.sender_zone, n.sender_subnet,
            "aurora-iap,aurora-tx",
        )
        self._create_instance(
            n.receiver_instance, self.config.receiver_zone, n.receiver_subnet,
            "aurora-iap,aurora-rx",
        )
        sender_ip = self._public_ip(n.sender_instance, self.config.sender_zone)
        receiver_ip = self._public_ip(
            n.receiver_instance, self.config.receiver_zone
        )
        self.gc(
            "compute", "firewall-rules", "create", n.forward_firewall,
            f"--network={n.receiver_network}", "--direction=INGRESS",
            "--action=ALLOW", f"--rules=udp:{FORWARD_PORT}",
            f"--source-ranges={sender_ip}/32", "--target-tags=aurora-rx",
            timeout=120,
        )
        self.gc(
            "compute", "firewall-rules", "create", n.reverse_firewall,
            f"--network={n.sender_network}", "--direction=INGRESS",
            "--action=ALLOW", f"--rules=udp:{REVERSE_PORT}",
            f"--source-ranges={receiver_ip}/32", "--target-tags=aurora-tx",
            timeout=120,
        )
        return sender_ip, receiver_ip

    def _create_instance(
        self, instance: str, zone: str, subnet: str, tags: str
    ) -> None:
        self.gc(
            "compute", "instances", "create", instance,
            f"--zone={zone}", f"--machine-type={self.config.machine_type}",
            f"--subnet={subnet}", "--network-tier=PREMIUM", f"--tags={tags}",
            "--image-family=ubuntu-2404-lts-amd64",
            "--image-project=ubuntu-os-cloud", "--boot-disk-size=10GB",
            "--boot-disk-type=pd-standard", "--max-run-duration=30m",
            "--instance-termination-action=DELETE", "--no-service-account",
            "--no-scopes", timeout=240,
        )

    def _public_ip(self, instance: str, zone: str) -> str:
        result = self.gc(
            "compute", "instances", "describe", instance, f"--zone={zone}",
            "--format=value(networkInterfaces[0].accessConfigs[0].natIP)",
            timeout=120,
        )
        value = result.stdout.strip()
        if not re.fullmatch(r"(?:[0-9]{1,3}\.){3}[0-9]{1,3}", value):
            raise HarnessError(f"invalid public IPv4 for {instance}")
        return value

    def prepare_runtime(self, temp: Path) -> str:
        n = self.names
        condition = CONDITION_PROFILES[self.config.condition_profile]
        ssh_keygen = shutil.which("ssh-keygen")
        if not ssh_keygen:
            raise HarnessError("ssh-keygen was not found")
        self.ssh_key_file = temp / "gcp-ssh-key"
        self.runner.run(
            [
                ssh_keygen,
                "-q",
                "-t",
                "rsa",
                "-b",
                "3072",
                "-N",
                "",
                "-f",
                str(self.ssh_key_file),
            ],
            timeout=30,
        )
        tx_setup = (
            "set -euo pipefail; "
            "sudo DEBIAN_FRONTEND=noninteractive apt-get update; "
            "sudo DEBIAN_FRONTEND=noninteractive apt-get install -y "
            "git cmake g++ make libsodium-dev; "
            f"git clone {self.config.repository_url} /tmp/aurora-src; "
            "cd /tmp/aurora-src; "
            f"git checkout {self.config.source_commit}; "
            "cmake -S . -B /tmp/aurora-build -DCMAKE_BUILD_TYPE=Release "
            "-DBUILD_TESTING=OFF -DBUILD_NET_TOOLS=OFF -DUSE_SODIUM=ON "
            "-DUSE_RAPTORQ=OFF -DUSE_WIREHAIR=OFF; "
            "cmake --build /tmp/aurora-build --target "
            "aurora_process_emulation --parallel 2; "
            "mkdir -p /tmp/aurora-runtime; "
            "cp /tmp/aurora-build/bin/aurora_process_emulation "
            "/tmp/aurora-runtime/; "
            f"cp {condition.forward_trace} {condition.reverse_trace} "
            "/tmp/aurora-runtime/"
        )
        rx_setup = (
            "set -euo pipefail; "
            "sudo DEBIAN_FRONTEND=noninteractive apt-get update; "
            "sudo DEBIAN_FRONTEND=noninteractive apt-get install -y libsodium23; "
            "mkdir -p /tmp/aurora-runtime"
        )
        with ThreadPoolExecutor(max_workers=2) as pool:
            futures = [
                pool.submit(
                    self.ssh, n.sender_instance, self.config.sender_zone,
                    tx_setup, timeout=900,
                ),
                pool.submit(
                    self.ssh, n.receiver_instance, self.config.receiver_zone,
                    rx_setup, timeout=600,
                ),
            ]
            for future in futures:
                future.result()

        runtime_dir = temp / "runtime"
        runtime_dir.mkdir()
        binary = runtime_dir / "aurora_process_emulation"
        forward_trace = runtime_dir / condition.forward_name
        reverse_trace = runtime_dir / condition.reverse_name
        for remote_name, local_path in (
            ("aurora_process_emulation", binary),
            (condition.forward_name, forward_trace),
            (condition.reverse_name, reverse_trace),
        ):
            self.scp(
                f"{n.sender_instance}:/tmp/aurora-runtime/{remote_name}",
                str(local_path), zone=self.config.sender_zone,
            )
        for local_path in (binary, forward_trace, reverse_trace):
            self.scp(
                str(local_path),
                f"{n.receiver_instance}:/tmp/aurora-runtime/{local_path.name}",
                zone=self.config.receiver_zone,
            )

        key_file = temp / "process-auth.key"
        session_file = temp / "process-session.id"
        key_file.write_text(secrets.token_hex(32) + "\n", encoding="ascii")
        session_file.write_text(secrets.token_hex(8) + "\n", encoding="ascii")
        try:
            os.chmod(key_file, 0o600)
            os.chmod(session_file, 0o600)
        except OSError:
            pass
        for instance, zone in (
            (n.sender_instance, self.config.sender_zone),
            (n.receiver_instance, self.config.receiver_zone),
        ):
            self.scp(str(key_file), f"{instance}:/tmp/process-auth.key", zone=zone)
            self.scp(
                str(session_file), f"{instance}:/tmp/process-session.id",
                zone=zone,
            )
            self.ssh(
                instance, zone,
                "chmod 700 /tmp/aurora-runtime/aurora_process_emulation; "
                "chmod 600 /tmp/process-auth.key /tmp/process-session.id",
                timeout=120,
            )
        return sha256_file(binary)

    def run_transport(
        self, sender_ip: str, receiver_ip: str
    ) -> tuple[str, str, dict]:
        n = self.names
        condition = CONDITION_PROFILES[self.config.condition_profile]
        evidence = self.config.evidence_dir
        evidence.mkdir(parents=True, exist_ok=True)
        receiver_log = evidence / "receiver.log"
        receiver_err = evidence / "receiver.stderr.log"
        sender_log = evidence / "sender.log"
        sender_err = evidence / "sender.stderr.log"
        receiver_command = (
            "set -euo pipefail; session=$(tr -d '\\r\\n' "
            "< /tmp/process-session.id); "
            "/tmp/aurora-runtime/aurora_process_emulation receiver "
            f"0.0.0.0 {FORWARD_PORT} {sender_ip} {REVERSE_PORT} "
            f"{WORKLOAD_GENERATIONS[self.config.workload_id]} "
            f"/tmp/aurora-runtime/{condition.reverse_name} "
            "/tmp/process-auth.key \"${session}\" "
            f"{STARTUP_TIMEOUT_MS} "
            f"{workload_service_timeout_ms(self.config.workload_id)}"
        )
        sender_command = (
            "set -euo pipefail; session=$(tr -d '\\r\\n' "
            "< /tmp/process-session.id); "
            "/tmp/aurora-runtime/aurora_process_emulation sender "
            f"{receiver_ip} {FORWARD_PORT} 0.0.0.0 {REVERSE_PORT} "
            f"/tmp/aurora-runtime/{condition.forward_name} "
            "/tmp/process-auth.key \"${session}\" "
            f"--policy {self.config.policy_id} "
            f"--workload {self.config.workload_id}"
        )
        started_ns = time.perf_counter_ns()
        with receiver_log.open("w", encoding="utf-8") as rx_out, \
                receiver_err.open("w", encoding="utf-8") as rx_err:
            receiver = self.runner.popen(
                self.ssh_args(
                    n.receiver_instance, self.config.receiver_zone,
                    receiver_command,
                ),
                stdout=rx_out,
                stderr=rx_err,
            )
            ready_ns = self._wait_ready(receiver, receiver_log, 90)
            sender_started_ns = time.perf_counter_ns()
            sender = self.runner.run(
                self.ssh_args(
                    n.sender_instance, self.config.sender_zone, sender_command
                ),
                check=False,
                timeout=(
                    180
                    if self.config.workload_id == "policy-pilot-v1"
                    else 90
                ),
            )
            sender_finished_ns = time.perf_counter_ns()
            sender_log.write_text(sender.stdout, encoding="utf-8")
            sender_err.write_text(sender.stderr, encoding="utf-8")
            if sender.returncode != 0:
                receiver.terminate()
                receiver.wait(timeout=10)
                raise HarnessError(f"sender exited {sender.returncode}")
            try:
                receiver_status = receiver.wait(timeout=30)
            except subprocess.TimeoutExpired as error:
                receiver.terminate()
                receiver.wait(timeout=10)
                raise HarnessError("receiver did not exit after sender") from error
        receiver_finished_ns = time.perf_counter_ns()
        if receiver_status != 0:
            raise HarnessError(f"receiver exited {receiver_status}")
        sender_text = sender_log.read_text(encoding="utf-8")
        receiver_text = receiver_log.read_text(encoding="utf-8")
        verified = verify_process_logs(
            sender_text,
            receiver_text,
            expected_generations=WORKLOAD_GENERATIONS[
                self.config.workload_id
            ],
            expected_policy_id=self.config.policy_id,
            expected_workload_id=self.config.workload_id,
            expected_service_timeout_ms=workload_service_timeout_ms(
                self.config.workload_id
            ),
        )
        verified.update(
            {
                "controller_receiver_ready_ms": round(
                    (ready_ns - started_ns) / 1_000_000, 3
                ),
                "controller_sender_wall_ms": round(
                    (sender_finished_ns - sender_started_ns) / 1_000_000, 3
                ),
                "controller_total_wall_ms": round(
                    (receiver_finished_ns - started_ns) / 1_000_000, 3
                ),
            }
        )
        return sender_text, receiver_text, verified

    @staticmethod
    def _wait_ready(
        process: subprocess.Popen[str], log_path: Path, timeout_seconds: int
    ) -> int:
        deadline = time.monotonic() + timeout_seconds
        while time.monotonic() < deadline:
            text = log_path.read_text(encoding="utf-8") if log_path.exists() else ""
            if any(line.startswith("receiver_ready ") for line in text.splitlines()):
                return time.perf_counter_ns()
            status = process.poll()
            if status is not None:
                raise HarnessError(
                    f"receiver exited {status} before publishing readiness"
                )
            time.sleep(0.2)
        process.terminate()
        process.wait(timeout=10)
        raise HarnessError("receiver readiness timed out at controller")

    def host_metadata(self, instance: str, zone: str) -> dict:
        command = (
            "set -euo pipefail; hostname; uname -srmo; "
            "timedatectl show -p NTPSynchronized --value; "
            "sha256sum /tmp/aurora-runtime/aurora_process_emulation | "
            "cut -d ' ' -f 1; date -u +%Y-%m-%dT%H:%M:%SZ"
        )
        lines = self.ssh(instance, zone, command, timeout=120).stdout.splitlines()
        if len(lines) != 5:
            raise HarnessError(f"unexpected metadata from {instance}")
        return {
            "hostname": lines[0],
            "kernel": lines[1],
            "ntp_synchronized": lines[2].strip().lower() == "yes",
            "binary_sha256": lines[3],
            "observed_at_utc": lines[4],
        }

    def cleanup(self) -> dict:
        n = self.names
        sender_region = zone_region(self.config.sender_zone)
        receiver_region = zone_region(self.config.receiver_zone)
        commands: list[list[str]] = [
            ["compute", "instances", "delete", n.sender_instance,
             f"--zone={self.config.sender_zone}", "--delete-disks=all", "--quiet"],
            ["compute", "instances", "delete", n.receiver_instance,
             f"--zone={self.config.receiver_zone}", "--delete-disks=all", "--quiet"],
            ["compute", "firewall-rules", "delete", n.forward_firewall,
             "--quiet"],
            ["compute", "firewall-rules", "delete", n.reverse_firewall,
             "--quiet"],
            ["compute", "firewall-rules", "delete", n.sender_iap_firewall,
             "--quiet"],
            ["compute", "firewall-rules", "delete", n.receiver_iap_firewall,
             "--quiet"],
            ["compute", "networks", "subnets", "delete", n.sender_subnet,
             f"--region={sender_region}", "--quiet"],
            ["compute", "networks", "subnets", "delete", n.receiver_subnet,
             f"--region={receiver_region}", "--quiet"],
            ["compute", "networks", "delete", n.sender_network, "--quiet"],
            ["compute", "networks", "delete", n.receiver_network, "--quiet"],
        ]
        cleanup_errors = []
        for command in commands:
            try:
                result = self.gc(*command, check=False, timeout=240)
            except Exception:
                cleanup_errors.append(command[1:4])
                continue
            if result.returncode != 0:
                cleanup_errors.append(command[1:4])
        remaining: dict[str, list[str]] = {}
        audits = {
            "instances": ["compute", "instances", "list"],
            "disks": ["compute", "disks", "list"],
            "addresses": ["compute", "addresses", "list"],
            "snapshots": ["compute", "snapshots", "list"],
            "firewall_rules": ["compute", "firewall-rules", "list"],
            "subnets": ["compute", "networks", "subnets", "list"],
            "networks": ["compute", "networks", "list"],
        }
        for resource, command in audits.items():
            try:
                result = self.gc(
                    *command, "--format=value(name)", check=False, timeout=120
                )
            except Exception:
                remaining[resource] = ["audit-unavailable"]
                continue
            if result.returncode != 0:
                remaining[resource] = ["audit-unavailable"]
                continue
            names = [
                value.strip() for value in result.stdout.splitlines()
                if value.strip().startswith(n.prefix)
            ]
            remaining[resource] = names
        return {
            "success": not any(remaining.values()),
            "delete_nonzero_count": len(cleanup_errors),
            "remaining": remaining,
        }

    def execute(self) -> dict:
        manifest = {
            "schema": RAW_EVIDENCE_SCHEMA,
            "evidence_level": "emulation",
            "project": self.config.project,
            "run_id": self.config.run_id,
            "source_commit": self.config.source_commit,
            "topology": "two-cross-region-non-peered-vpcs-public-ipv4",
            "machine_type": self.config.machine_type,
            "treatment": {
                "policy_id": self.config.policy_id,
                "selection": "sender-runtime-argument",
                "binary_rebuild_per_treatment": False,
            },
            "condition": {
                "profile": self.config.condition_profile,
                "forward_trace": CONDITION_PROFILES[
                    self.config.condition_profile
                ].forward_name,
                "reverse_trace": CONDITION_PROFILES[
                    self.config.condition_profile
                ].reverse_name,
            },
            "workload": {
                "id": self.config.workload_id,
                "generation_count": WORKLOAD_GENERATIONS[
                    self.config.workload_id
                ],
                "startup_timeout_ms": STARTUP_TIMEOUT_MS,
                "service_timeout_ms": workload_service_timeout_ms(
                    self.config.workload_id
                ),
            },
            "measurement": measurement_contract(),
            "result": "failed",
        }
        self.config.evidence_dir.mkdir(parents=True, exist_ok=True)
        failure: BaseException | None = None
        try:
            sender_ip, receiver_ip = self.provision()
            with tempfile.TemporaryDirectory(prefix="aurora-gcp-") as temp_name:
                binary_sha = self.prepare_runtime(Path(temp_name))
                sender_text, receiver_text, timing = self.run_transport(
                    sender_ip, receiver_ip
                )
                sender_meta = self.host_metadata(
                    self.names.sender_instance, self.config.sender_zone
                )
                receiver_meta = self.host_metadata(
                    self.names.receiver_instance, self.config.receiver_zone
                )
            if sender_meta["binary_sha256"] != binary_sha or \
                    receiver_meta["binary_sha256"] != binary_sha:
                raise HarnessError("runtime binary identity mismatch")
            manifest.update(
                {
                    "result": "passed",
                    "authentication_profile": "hmac-sha256-libsodium",
                    "ports": {
                        "forward": FORWARD_PORT,
                        "reverse": REVERSE_PORT,
                    },
                    "runtime_binary_sha256": binary_sha,
                    "sender": {
                        "zone": self.config.sender_zone,
                        "public_ipv4": sender_ip,
                        **sender_meta,
                    },
                    "receiver": {
                        "zone": self.config.receiver_zone,
                        "public_ipv4": receiver_ip,
                        **receiver_meta,
                    },
                    "timing": timing,
                    "logs": {
                        "sender_sha256": hashlib.sha256(
                            sender_text.encode("utf-8")
                        ).hexdigest(),
                        "receiver_sha256": hashlib.sha256(
                            receiver_text.encode("utf-8")
                        ).hexdigest(),
                    },
                    "claims": {
                        "calibrated_performance": False,
                        "field_evidence": False,
                        "timing_scope": TIMING_SCOPE,
                        "one_way_latency": False,
                        "feedback_rtt_network_only": False,
                    },
                }
            )
        except BaseException as error:
            failure = error
            manifest["failure_type"] = type(error).__name__
            manifest["failure"] = str(error)[-1000:]
        finally:
            manifest["teardown"] = self.cleanup()
            (self.config.evidence_dir / "manifest.json").write_text(
                json.dumps(manifest, indent=2, sort_keys=True) + "\n",
                encoding="utf-8",
            )
        if failure is not None:
            raise failure
        if not manifest["teardown"]["success"]:
            raise HarnessError("transport passed but teardown audit failed")
        return manifest


def current_commit() -> str:
    result = subprocess.run(
        ["git", "rev-parse", "HEAD"], capture_output=True, text=True,
        check=False,
    )
    return result.stdout.strip()


def default_run_id() -> str:
    return time.strftime("run-%y%m%d-%H%M%S", time.gmtime())


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--project", required=True)
    result.add_argument("--run-id", default=default_run_id())
    result.add_argument("--sender-zone", default="us-east1-b")
    result.add_argument("--receiver-zone", default="us-west1-b")
    result.add_argument("--machine-type", default="e2-micro")
    result.add_argument(
        "--condition-profile",
        choices=sorted(CONDITION_PROFILES),
        default="timed-replay-v2",
    )
    result.add_argument(
        "--policy-id", choices=POLICY_IDS, default="biological-adaptive"
    )
    result.add_argument(
        "--workload-id",
        choices=sorted(WORKLOAD_GENERATIONS),
        default="smoke-v2",
    )
    result.add_argument("--source-commit", default=current_commit())
    result.add_argument(
        "--repository-url",
        default="https://github.com/Daniele-Cangi/Aurora.git",
    )
    result.add_argument("--evidence-dir", type=Path)
    result.add_argument("--execute", action="store_true")
    result.add_argument("--cleanup-only", action="store_true")
    result.add_argument(
        "--acknowledge-billing-and-teardown", action="store_true"
    )
    result.add_argument("--quiet", action="store_true")
    return result


def main(argv: Iterable[str] | None = None) -> int:
    args = parser().parse_args(argv)
    evidence_dir = args.evidence_dir or Path("raw-host-evidence") / args.run_id
    config = Config(
        project=args.project,
        run_id=args.run_id,
        sender_zone=args.sender_zone,
        receiver_zone=args.receiver_zone,
        machine_type=args.machine_type,
        condition_profile=args.condition_profile,
        source_commit=args.source_commit,
        repository_url=args.repository_url,
        evidence_dir=evidence_dir,
        policy_id=args.policy_id,
        workload_id=args.workload_id,
    )
    config.validate()
    names = Names.from_run_id(config.run_id)
    plan = build_plan(config, names)
    if not args.execute and not args.cleanup_only:
        print(json.dumps(plan, indent=2, sort_keys=True))
        return 0
    if not args.acknowledge_billing_and_teardown:
        raise HarnessError(
            "execution and cleanup require --acknowledge-billing-and-teardown"
        )
    gcloud = shutil.which("gcloud")
    if not gcloud:
        raise HarnessError("gcloud was not found")
    harness = GcpRawHarness(
        config, CommandRunner(verbose=not args.quiet), gcloud=gcloud
    )
    if args.cleanup_only:
        cleanup = harness.cleanup()
        evidence_dir.mkdir(parents=True, exist_ok=True)
        (evidence_dir / "cleanup.json").write_text(
            json.dumps(cleanup, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        return 0 if cleanup["success"] else 1

    def stop_handler(_signum, _frame):
        raise KeyboardInterrupt("termination requested; cleaning resources")

    if hasattr(signal, "SIGTERM"):
        signal.signal(signal.SIGTERM, stop_handler)
    manifest = harness.execute()
    print(json.dumps(manifest, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except HarnessError as error:
        print(f"gcp raw host emulation: {error}", file=sys.stderr)
        raise SystemExit(1)
