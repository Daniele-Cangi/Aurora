#!/usr/bin/env python3
"""Validate one raw-host post-shock policy-study lifecycle."""

from __future__ import annotations

import hashlib
import json
import math
from pathlib import Path


GENERATION_COUNT = 8
SHOCK_GENERATION = 2
POST_SHOCK_GENERATIONS = (3, 4, 5, 6, 7)
POLICIES = ("fixed-class-aware", "biological-adaptive")
DEADLINE_SEMANTICS = "receiver-steady-descriptor-relative"


class EvidenceError(RuntimeError):
    pass


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(block)
    except OSError as error:
        raise EvidenceError(f"cannot hash evidence file: {path}") from error
    return digest.hexdigest()


def parse_record(line: str) -> dict[str, str]:
    fields: dict[str, str] = {}
    for token in line.split()[1:]:
        if "=" in token:
            key, value = token.split("=", 1)
            fields[key] = value
    return fields


def records(text: str, prefix: str) -> list[dict[str, str]]:
    return [
        parse_record(line)
        for line in text.splitlines()
        if line.startswith(prefix + " ")
    ]


def integer(record: dict[str, str], key: str) -> int:
    try:
        return int(record[key])
    except (KeyError, ValueError) as error:
        raise EvidenceError(f"invalid or missing integer {key}: {record}") from error


def real(record: dict[str, str], key: str) -> float:
    try:
        return float(record[key])
    except (KeyError, ValueError) as error:
        raise EvidenceError(f"invalid or missing number {key}: {record}") from error


def indexed(value: list[dict[str, str]], role: str) -> dict[int, dict[str, str]]:
    result = {integer(record, "index"): record for record in value}
    expected = set(range(GENERATION_COUNT))
    if len(value) != GENERATION_COUNT or set(result) != expected:
        raise EvidenceError(f"{role} evidence is not the fixed eight generations")
    return result


def protection(record: dict[str, str]) -> tuple[float, float, float]:
    return tuple(
        real(record, key)
        for key in (
            "critical_protection_factor",
            "important_protection_factor",
            "elastic_protection_factor",
        )
    )


def load_manifest(evidence_dir: Path) -> dict:
    path = evidence_dir / "manifest.json"
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise EvidenceError(f"invalid evidence manifest: {path}") from error
    if not isinstance(value, dict):
        raise EvidenceError(f"evidence manifest is not an object: {path}")
    return value


def _validate_manifest(
    evidence_dir: Path,
    manifest: dict,
    *,
    run_id: str,
    source_commit: str,
    policy: str,
) -> None:
    if manifest.get("schema") != "aurora-raw-host-evidence-v4" or \
            manifest.get("result") != "passed":
        raise EvidenceError(f"{run_id}: lifecycle evidence did not pass")
    if manifest.get("run_id") != run_id or \
            manifest.get("source_commit") != source_commit:
        raise EvidenceError(f"{run_id}: lifecycle identity mismatch")
    if manifest.get("process_protocol_version") != 3 or \
            manifest.get("terminal_handshake") != "authenticated-ack-v1" or \
            manifest.get("authentication_profile") != "hmac-sha256-libsodium":
        raise EvidenceError(f"{run_id}: authenticated transport contract changed")
    if manifest.get("topology") != \
            "two-cross-region-non-peered-vpcs-public-ipv4" or \
            manifest.get("workload", {}).get("id") != "policy-pilot-v1" or \
            manifest.get("workload", {}).get("generation_count") != 8 or \
            manifest.get("condition", {}).get("profile") != "regime-change-v1" or \
            manifest.get("treatment", {}).get("policy_id") != policy:
        raise EvidenceError(f"{run_id}: frozen study factor mismatch")
    if manifest.get("measurement", {}).get("cross_clock_subtraction_permitted") \
            is not False:
        raise EvidenceError(f"{run_id}: cross-clock subtraction was permitted")
    if manifest.get("teardown", {}).get("success") is not True or \
            manifest.get("teardown", {}).get("delete_nonzero_count") != 0:
        raise EvidenceError(f"{run_id}: lifecycle teardown was not clean")
    runtime_hash = manifest.get("runtime_binary_sha256")
    if not isinstance(runtime_hash, str) or len(runtime_hash) != 64 or \
            manifest.get("sender", {}).get("binary_sha256") != runtime_hash or \
            manifest.get("receiver", {}).get("binary_sha256") != runtime_hash:
        raise EvidenceError(f"{run_id}: runtime binary identity mismatch")

    logs = manifest.get("logs", {})
    for role in ("sender", "receiver"):
        log_path = evidence_dir / f"{role}.log"
        if sha256_file(log_path) != logs.get(f"{role}_sha256"):
            raise EvidenceError(f"{run_id}: {role} log checksum mismatch")
        stderr_path = evidence_dir / f"{role}.stderr.log"
        try:
            stderr_size = stderr_path.stat().st_size
        except OSError as error:
            raise EvidenceError(f"{run_id}: missing {role} stderr evidence") from error
        if stderr_size != 0:
            raise EvidenceError(f"{run_id}: {role} stderr was not empty")


def _validate_generations(
    sender: dict[int, dict[str, str]],
    receiver: dict[int, dict[str, str]],
    *,
    policy: str,
) -> None:
    adaptive_fields = (
        "adaptive_state_present",
        "adaptive_generation_count_at_plan",
        "adaptive_success_count_at_plan",
        "adaptive_failure_count_at_plan",
        "adaptive_panic_boost_at_plan",
        "adaptive_critical_overhead_at_plan",
        "adaptive_important_overhead_at_plan",
        "adaptive_success_count_after_terminal",
        "adaptive_failure_count_after_terminal",
        "adaptive_panic_boost_after_terminal",
    )
    for index in range(GENERATION_COUNT):
        sent = sender[index]
        received = receiver[index]
        if sent.get("policy_id") != policy:
            raise EvidenceError(f"generation {index}: wrong policy identity")
        if integer(sent, "source_symbols") != 40:
            raise EvidenceError(f"generation {index}: workload source size changed")
        for key in ("initial_symbols", "repair_requested", "repair_emitted"):
            if integer(sent, key) < 0:
                raise EvidenceError(f"generation {index}: negative {key}")
        protection(sent)
        for key in adaptive_fields:
            real(sent, key)
        for key in ("delivered", "terminal_failure", "critical_before_deadline"):
            if integer(sent, key) != integer(received, key):
                raise EvidenceError(
                    f"generation {index}: sender/receiver {key} disagreement"
                )

        descriptor_at = integer(received, "descriptor_received_at_ms")
        critical_duration = integer(received, "critical_deadline_duration_ms")
        generation_duration = integer(received, "generation_deadline_duration_ms")
        if received.get("deadline_clock") != DEADLINE_SEMANTICS or \
                critical_duration != 10_000 or generation_duration != 30_000 or \
                integer(received, "critical_deadline_at_ms") != \
                descriptor_at + critical_duration or \
                integer(received, "generation_deadline_at_ms") != \
                descriptor_at + generation_duration:
            raise EvidenceError(
                f"generation {index}: receiver-relative deadline semantics changed"
            )
        expected_outage = 1 if index == SHOCK_GENERATION else 0
        if integer(received, "regime_outage") != expected_outage:
            raise EvidenceError(f"generation {index}: wrong regime outage schedule")

    failed = sender[SHOCK_GENERATION]
    receiver_failed = receiver[SHOCK_GENERATION]
    for index in (0, 1):
        if integer(sender[index], "delivered") != 1 or \
                integer(sender[index], "critical_before_deadline") != 1:
            raise EvidenceError(
                f"generation {index}: pre-shock control state was not successful"
            )
    if integer(failed, "delivered") != 0 or \
            integer(failed, "terminal_failure") != 1 or \
            integer(failed, "critical_before_deadline") != 0 or \
            integer(receiver_failed, "regime_suppressed_symbols") <= 0:
        raise EvidenceError("generation 2 did not contain the imposed terminal failure")

    plans = {protection(record) for record in sender.values()}
    if policy == "fixed-class-aware":
        expected = (2.5, 1.5, 1.1)
        if len(plans) != 1 or any(
            not math.isclose(actual, frozen, rel_tol=0.0, abs_tol=1e-12)
            for actual, frozen in zip(next(iter(plans)), expected)
        ) or any(integer(record, "adaptive_state_present") != 0
                 for record in sender.values()):
            raise EvidenceError("fixed-class-aware protection was not invariant")
    else:
        if any(integer(record, "adaptive_state_present") != 1
               for record in sender.values()):
            raise EvidenceError("biological policy omitted adaptive state")
        subsequent = sender[SHOCK_GENERATION + 1]
        if protection(subsequent) == protection(failed) or \
                integer(failed, "adaptive_failure_count_after_terminal") != 1 or \
                integer(subsequent, "adaptive_failure_count_at_plan") != 1 or \
                integer(subsequent, "adaptive_panic_boost_at_plan") != 2:
            raise EvidenceError("biological generation-3 plan ignored the failure")


def validate_lifecycle(
    evidence_dir: Path,
    *,
    run_id: str,
    source_commit: str,
    policy: str,
) -> dict:
    """Validate technical identity and return frozen per-lifecycle outcomes."""
    if policy not in POLICIES:
        raise EvidenceError(f"unsupported study policy: {policy}")
    manifest = load_manifest(evidence_dir)
    _validate_manifest(
        evidence_dir,
        manifest,
        run_id=run_id,
        source_commit=source_commit,
        policy=policy,
    )
    try:
        sender_text = (evidence_dir / "sender.log").read_text(encoding="utf-8")
        receiver_text = (evidence_dir / "receiver.log").read_text(encoding="utf-8")
    except OSError as error:
        raise EvidenceError(f"{run_id}: cannot read lifecycle logs") from error
    sender = indexed(records(sender_text, "sender_generation_complete"), "sender")
    receiver = indexed(
        records(receiver_text, "receiver_generation_complete"), "receiver"
    )
    _validate_generations(sender, receiver, policy=policy)

    post = [sender[index] for index in POST_SHOCK_GENERATIONS]
    timing = manifest.get("timing", {})
    for key in (
        "terminal_feedback_rtt_mean_us",
        "terminal_feedback_retry_rounds",
    ):
        if not isinstance(timing.get(key), (int, float)):
            raise EvidenceError(f"{run_id}: missing lifecycle metric {key}")
    generations = []
    for index in range(GENERATION_COUNT):
        record = sender[index]
        generations.append({
            "index": index,
            "delivered": integer(record, "delivered"),
            "terminal_failure": integer(record, "terminal_failure"),
            "critical_before_deadline": integer(
                record, "critical_before_deadline"
            ),
            "source_symbols": integer(record, "source_symbols"),
            "initial_symbols": integer(record, "initial_symbols"),
            "repair_symbols_requested": integer(record, "repair_requested"),
            "repair_symbols_emitted": integer(record, "repair_emitted"),
            "wire_symbol_datagrams": integer(record, "initial_symbols") +
            integer(record, "repair_emitted"),
            "protection_factors": list(protection(record)),
            "adaptive_state_present": integer(record, "adaptive_state_present"),
            "adaptive_success_count_at_plan": integer(
                record, "adaptive_success_count_at_plan"
            ),
            "adaptive_failure_count_at_plan": integer(
                record, "adaptive_failure_count_at_plan"
            ),
            "adaptive_panic_boost_at_plan": integer(
                record, "adaptive_panic_boost_at_plan"
            ),
        })
    return {
        "run_id": run_id,
        "policy": policy,
        "runtime_binary_sha256": manifest["runtime_binary_sha256"],
        "post_shock": {
            "scheduled_generations": len(POST_SHOCK_GENERATIONS),
            "critical_before_deadline": sum(
                integer(record, "critical_before_deadline") for record in post
            ),
            "delivered": sum(integer(record, "delivered") for record in post),
            "source_symbols": sum(
                integer(record, "source_symbols") for record in post
            ),
            "initial_symbols": sum(
                integer(record, "initial_symbols") for record in post
            ),
            "repair_symbols_requested": sum(
                integer(record, "repair_requested") for record in post
            ),
            "repair_symbols_emitted": sum(
                integer(record, "repair_emitted") for record in post
            ),
            "wire_symbol_datagrams": sum(
                integer(record, "initial_symbols") +
                integer(record, "repair_emitted") for record in post
            ),
        },
        "whole_lifecycle": {
            "terminal_feedback_rtt_mean_us": timing[
                "terminal_feedback_rtt_mean_us"
            ],
            "terminal_feedback_retry_rounds": timing[
                "terminal_feedback_retry_rounds"
            ],
        },
        "generations": generations,
    }
