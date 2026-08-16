import socket
import subprocess
import sys
import re

from policy_pilot_evidence import verify_case


def free_port(excluded=None):
    while True:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.bind(("127.0.0.1", 0))
            port = sock.getsockname()[1]
        if port != excluded:
            return port


def main():
    if not 6 <= len(sys.argv) <= 11:
        raise SystemExit(
            "usage: run_process_emulation.py <emulator> <forward-trace> "
            "<reverse-trace> <key-file> <session-id-hex> "
            "[timed|zero-delay] [policy-id workload-id generation-count "
            "[timed-replay-v2|regime-change-v1]]"
        )

    executable = sys.argv[1]
    trace = sys.argv[2]
    reverse_trace = sys.argv[3]
    key_file = sys.argv[4]
    session_id = sys.argv[5]
    delay_mode = sys.argv[6] if len(sys.argv) >= 7 else "timed"
    if delay_mode not in ("timed", "zero-delay"):
        raise SystemExit("delay mode must be timed or zero-delay")
    policy_id = sys.argv[7] if len(sys.argv) >= 8 else None
    workload_id = sys.argv[8] if len(sys.argv) >= 9 else None
    expected_generations = int(sys.argv[9]) if len(sys.argv) >= 10 else 2
    condition_id = sys.argv[10] if len(sys.argv) >= 11 else "timed-replay-v2"
    if condition_id not in ("timed-replay-v2", "regime-change-v1"):
        raise SystemExit("unknown policy pilot condition")
    if (policy_id is None) != (workload_id is None):
        raise SystemExit("policy-id and workload-id must be supplied together")
    service_timeout_ms = 120000 if workload_id == "policy-pilot-v1" else 15000
    forward_port = free_port()
    feedback_port = free_port(forward_port)
    receiver_command = [
            executable,
            "receiver",
            "127.0.0.1",
            str(forward_port),
            "127.0.0.1",
            str(feedback_port),
            str(expected_generations),
            reverse_trace,
            key_file,
            session_id,
        ]
    if workload_id == "policy-pilot-v1":
        receiver_command.extend(["60000", str(service_timeout_ms)])
        if condition_id == "regime-change-v1":
            receiver_command.extend(["--outage-generation", "2"])
    receiver = subprocess.Popen(
        receiver_command,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )

    try:
        ready_line = receiver.stdout.readline()
        if "receiver_ready" not in ready_line:
            receiver_stdout, receiver_stderr = receiver.communicate(timeout=3)
            raise RuntimeError(
                "receiver did not publish readiness\n"
                f"stdout:\n{ready_line}{receiver_stdout}\n"
                f"stderr:\n{receiver_stderr}"
            )
        sender_command = [
                executable,
                "sender",
                "127.0.0.1",
                str(forward_port),
                "127.0.0.1",
                str(feedback_port),
                trace,
                key_file,
                session_id,
            ]
        if policy_id is not None:
            sender_command.extend(
                ["--policy", policy_id, "--workload", workload_id]
            )
        sender = subprocess.run(
            sender_command,
            capture_output=True,
            text=True,
            timeout=180 if workload_id == "policy-pilot-v1" else 20,
            check=False,
        )
        if sender.returncode != 0:
            raise RuntimeError(
                f"sender failed ({sender.returncode})\n"
                f"stdout:\n{sender.stdout}\nstderr:\n{sender.stderr}"
            )
        receiver_tail, receiver_stderr = receiver.communicate(
            timeout=30 if workload_id == "policy-pilot-v1" else 10
        )
        receiver_stdout = ready_line + receiver_tail
        if receiver.returncode != 0:
            raise RuntimeError(
                f"receiver failed ({receiver.returncode})\n"
                f"stdout:\n{receiver_stdout}\nstderr:\n{receiver_stderr}"
            )
        if "sender_complete" not in sender.stdout:
            raise RuntimeError(f"missing sender evidence: {sender.stdout}")
        if (
            f"generations={expected_generations}" not in sender.stdout
            or f"feedback_applied={expected_generations}" not in sender.stdout
        ):
            raise RuntimeError(f"reverse feedback was not applied: {sender.stdout}")
        if policy_id is not None and (
            f"policy_id={policy_id}" not in sender.stdout
            or f"workload_id={workload_id}" not in sender.stdout
        ):
            raise RuntimeError(f"runtime treatment was not reported: {sender.stdout}")
        if "protocol_version=3" not in sender.stdout:
            raise RuntimeError(f"wrong process protocol version: {sender.stdout}")
        if (
            f"terminal_ack_datagrams={expected_generations * 3}"
            not in sender.stdout
            or "terminal_handshake=authenticated-ack-v1" not in sender.stdout
        ):
            raise RuntimeError(
                f"missing authenticated terminal acknowledgements: {sender.stdout}"
            )
        rtt = {
            name: re.search(rf"{name}=(\d+)", sender.stdout)
            for name in (
                "feedback_rtt_samples",
                "feedback_rtt_min_us",
                "feedback_rtt_mean_us",
                "feedback_rtt_max_us",
                "terminal_feedback_rtt_samples",
                "terminal_feedback_rtt_min_us",
                "terminal_feedback_rtt_mean_us",
                "terminal_feedback_rtt_max_us",
                "unknown_feedback_echoes",
            )
        }
        if any(value is None for value in rtt.values()):
            raise RuntimeError(f"missing sender-clock RTT evidence: {sender.stdout}")
        rtt_values = {name: int(value.group(1)) for name, value in rtt.items()}
        if rtt_values["feedback_rtt_samples"] < expected_generations:
            raise RuntimeError(f"too few feedback RTT samples: {sender.stdout}")
        if rtt_values["terminal_feedback_rtt_samples"] != expected_generations:
            raise RuntimeError(f"wrong terminal RTT sample count: {sender.stdout}")
        if rtt_values["unknown_feedback_echoes"] != 0:
            raise RuntimeError(f"unbound feedback echo accepted: {sender.stdout}")
        for prefix in ("feedback_rtt", "terminal_feedback_rtt"):
            minimum = rtt_values[f"{prefix}_min_us"]
            mean = rtt_values[f"{prefix}_mean_us"]
            maximum = rtt_values[f"{prefix}_max_us"]
            if minimum <= 0 or not minimum <= mean <= maximum:
                raise RuntimeError(f"invalid {prefix} summary: {sender.stdout}")
        stale_feedback = re.search(
            r"stale_feedback_datagrams=(\d+)", sender.stdout
        )
        if delay_mode == "timed" and (
            not stale_feedback or int(stale_feedback.group(1)) == 0
        ):
            raise RuntimeError(
                f"reordered feedback was not rejected monotonically: {sender.stdout}"
            )
        sender_replay = re.search(r"replay_rejected=(\d+)", sender.stdout)
        if not sender_replay or int(sender_replay.group(1)) == 0:
            raise RuntimeError(f"reverse replay was not rejected: {sender.stdout}")
        delayed = re.search(r"impairment_delayed=(\d+)", sender.stdout)
        reordered = re.search(r"impairment_reordered=(\d+)", sender.stdout)
        if delay_mode == "timed":
            if (
                not delayed
                or not reordered
                or int(delayed.group(1)) == 0
                or int(reordered.group(1)) == 0
            ):
                raise RuntimeError(
                    f"missing timed impairment evidence: {sender.stdout}"
                )
        elif not delayed or int(delayed.group(1)) != 0:
            raise RuntimeError(
                f"zero-delay trace added sender delay: {sender.stdout}"
            )
        if receiver_stdout.count("receiver_generation_complete") != expected_generations:
            raise RuntimeError(f"missing per-generation evidence: {receiver_stdout}")
        if f"receiver_complete generations={expected_generations}" not in receiver_stdout:
            raise RuntimeError(f"missing receiver evidence: {receiver_stdout}")
        if receiver_stdout.count("protocol_version=3") < 2:
            raise RuntimeError(
                f"wrong receiver protocol version evidence: {receiver_stdout}"
            )
        if (
            f"terminal_acknowledged={expected_generations}"
            not in receiver_stdout
            or receiver_stdout.count(
                "terminal_handshake=authenticated-ack-v1"
            ) < 2
        ):
            raise RuntimeError(
                f"receiver did not attest terminal acknowledgements: "
                f"{receiver_stdout}"
            )
        if "startup_timeout_ms=60000" not in receiver_stdout:
            raise RuntimeError(f"missing startup timeout evidence: {receiver_stdout}")
        if f"service_timeout_ms={service_timeout_ms}" not in receiver_stdout:
            raise RuntimeError(f"missing service timeout evidence: {receiver_stdout}")
        sender_elapsed = re.search(r"sender_elapsed_ms=(\d+)", sender.stdout)
        receiver_elapsed = re.search(
            r"service_elapsed_ms=(\d+)", receiver_stdout
        )
        if not sender_elapsed or int(sender_elapsed.group(1)) == 0:
            raise RuntimeError(f"missing sender timing evidence: {sender.stdout}")
        if not receiver_elapsed or int(receiver_elapsed.group(1)) == 0:
            raise RuntimeError(
                f"missing receiver timing evidence: {receiver_stdout}"
            )
        receiver_replay = re.search(
            r"replay_rejected=(\d+)", receiver_stdout
        )
        if not receiver_replay or int(receiver_replay.group(1)) == 0:
            raise RuntimeError(
                f"forward replay was not rejected: {receiver_stdout}"
            )
        if "auth_profile=" not in sender.stdout or "auth_profile=" not in receiver_stdout:
            raise RuntimeError("missing authentication profile evidence")
        if workload_id == "policy-pilot-v1":
            sender_summary = re.search(
                r"sender_complete .*critical_generations=(\d+) "
                r"critical_delivered_before_deadline=(\d+).*"
                r"source_symbols=(\d+) initial_symbols=(\d+) "
                r"repair_symbols_requested=(\d+) "
                r"repair_symbols_emitted=(\d+)",
                sender.stdout,
            )
            receiver_summary = re.search(
                r"receiver_complete .*critical_generations=(\d+) "
                r"critical_delivered_before_deadline=(\d+)",
                receiver_stdout,
            )
            if not sender_summary or not receiver_summary:
                raise RuntimeError("missing pilot outcome schema")
            if int(sender_summary.group(1)) != expected_generations:
                raise RuntimeError("wrong sender critical generation count")
            if int(receiver_summary.group(1)) != expected_generations:
                raise RuntimeError("wrong receiver critical generation count")
            if int(sender_summary.group(3)) <= 0 or int(sender_summary.group(4)) <= 0:
                raise RuntimeError("missing pilot redundancy counters")
            generation_records = re.findall(
                r"^sender_generation_complete .*initial_symbols=(\d+) ",
                sender.stdout,
                re.MULTILINE,
            )
            if len(generation_records) != expected_generations:
                raise RuntimeError("missing per-generation sender outcomes")
            verify_case(
                sender.stdout,
                receiver_stdout,
                policy_id,
                condition_id,
            )
        reverse_delayed = re.search(r"reverse_delayed=(\d+)", receiver_stdout)
        reverse_reordered = re.search(r"reverse_reordered=(\d+)", receiver_stdout)
        reverse_dropped = re.search(r"reverse_dropped=(\d+)", receiver_stdout)
        reverse_duplicated = re.search(r"reverse_duplicated=(\d+)", receiver_stdout)
        required_reverse = (reverse_dropped, reverse_duplicated)
        if any(
            value is None or int(value.group(1)) == 0
            for value in required_reverse
        ):
            raise RuntimeError(
                f"missing reverse impairment evidence: {receiver_stdout}"
            )
        if delay_mode == "timed":
            timed_reverse = (reverse_delayed, reverse_reordered)
            if any(
                value is None or int(value.group(1)) == 0
                for value in timed_reverse
            ):
                raise RuntimeError(
                    f"missing reverse timing evidence: {receiver_stdout}"
                )
        elif not reverse_delayed or int(reverse_delayed.group(1)) != 0:
            raise RuntimeError(
                f"zero-delay trace added receiver delay: {receiver_stdout}"
            )
        print(sender.stdout.strip())
        print(receiver_stdout.strip())
    finally:
        if receiver.poll() is None:
            receiver.terminate()
            try:
                receiver.wait(timeout=3)
            except subprocess.TimeoutExpired:
                receiver.kill()
                receiver.wait(timeout=3)


if __name__ == "__main__":
    main()
