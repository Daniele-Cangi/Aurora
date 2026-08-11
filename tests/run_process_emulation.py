import socket
import subprocess
import sys
import re


def free_port(excluded=None):
    while True:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.bind(("127.0.0.1", 0))
            port = sock.getsockname()[1]
        if port != excluded:
            return port


def main():
    if len(sys.argv) not in (6, 7):
        raise SystemExit(
            "usage: run_process_emulation.py <emulator> <forward-trace> "
            "<reverse-trace> <key-file> <session-id-hex> "
            "[timed|zero-delay]"
        )

    executable = sys.argv[1]
    trace = sys.argv[2]
    reverse_trace = sys.argv[3]
    key_file = sys.argv[4]
    session_id = sys.argv[5]
    delay_mode = sys.argv[6] if len(sys.argv) == 7 else "timed"
    if delay_mode not in ("timed", "zero-delay"):
        raise SystemExit("delay mode must be timed or zero-delay")
    forward_port = free_port()
    feedback_port = free_port(forward_port)
    receiver = subprocess.Popen(
        [
            executable,
            "receiver",
            "127.0.0.1",
            str(forward_port),
            "127.0.0.1",
            str(feedback_port),
            "2",
            reverse_trace,
            key_file,
            session_id,
        ],
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
        sender = subprocess.run(
            [
                executable,
                "sender",
                "127.0.0.1",
                str(forward_port),
                "127.0.0.1",
                str(feedback_port),
                trace,
                key_file,
                session_id,
            ],
            capture_output=True,
            text=True,
            timeout=20,
            check=False,
        )
        if sender.returncode != 0:
            raise RuntimeError(
                f"sender failed ({sender.returncode})\n"
                f"stdout:\n{sender.stdout}\nstderr:\n{sender.stderr}"
            )
        receiver_tail, receiver_stderr = receiver.communicate(timeout=10)
        receiver_stdout = ready_line + receiver_tail
        if receiver.returncode != 0:
            raise RuntimeError(
                f"receiver failed ({receiver.returncode})\n"
                f"stdout:\n{receiver_stdout}\nstderr:\n{receiver_stderr}"
            )
        if "sender_complete" not in sender.stdout:
            raise RuntimeError(f"missing sender evidence: {sender.stdout}")
        if "generations=2" not in sender.stdout or "feedback_applied=2" not in sender.stdout:
            raise RuntimeError(f"reverse feedback was not applied: {sender.stdout}")
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
        if receiver_stdout.count("receiver_generation_complete") != 2:
            raise RuntimeError(f"missing per-generation evidence: {receiver_stdout}")
        if "receiver_complete generations=2" not in receiver_stdout:
            raise RuntimeError(f"missing receiver evidence: {receiver_stdout}")
        if "startup_timeout_ms=60000" not in receiver_stdout:
            raise RuntimeError(f"missing startup timeout evidence: {receiver_stdout}")
        if "service_timeout_ms=15000" not in receiver_stdout:
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
