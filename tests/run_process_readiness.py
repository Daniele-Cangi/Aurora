import re
import socket
import subprocess
import sys
import time


def free_port(excluded=None):
    while True:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.bind(("127.0.0.1", 0))
            port = sock.getsockname()[1]
        if port != excluded:
            return port


def receiver_command(executable, forward_port, feedback_port, trace,
                     key_file, session_id, startup_ms, service_ms):
    return [
        executable,
        "receiver",
        "127.0.0.1",
        str(forward_port),
        "127.0.0.1",
        str(feedback_port),
        "2",
        trace,
        key_file,
        session_id,
        str(startup_ms),
        str(service_ms),
    ]


def main():
    if len(sys.argv) != 5:
        raise SystemExit(
            "usage: run_process_readiness.py <emulator> <trace> "
            "<key-file> <session-id-hex>"
        )

    executable, trace, key_file, session_id = sys.argv[1:]
    forward_port = free_port()
    feedback_port = free_port(forward_port)
    receiver = subprocess.Popen(
        receiver_command(
            executable,
            forward_port,
            feedback_port,
            trace,
            key_file,
            session_id,
            10000,
            5000,
        ),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        ready_line = receiver.stdout.readline()
        if "receiver_ready" not in ready_line:
            raise RuntimeError(f"missing readiness evidence: {ready_line}")
        if "startup_timeout_ms=10000" not in ready_line:
            raise RuntimeError(f"wrong startup timeout: {ready_line}")
        if "service_timeout_ms=5000" not in ready_line:
            raise RuntimeError(f"wrong service timeout: {ready_line}")
        if "protocol_version=3" not in ready_line:
            raise RuntimeError(f"wrong process protocol version: {ready_line}")
        if "terminal_handshake=authenticated-ack-v1" not in ready_line:
            raise RuntimeError(f"missing terminal handshake: {ready_line}")

        # This delay exceeds the service budget. Success proves that service
        # accounting begins at the first valid descriptor, not at bind time.
        time.sleep(5.5)
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
                f"delayed sender failed ({sender.returncode})\n"
                f"stdout:\n{sender.stdout}\nstderr:\n{sender.stderr}"
            )
        receiver_tail, receiver_stderr = receiver.communicate(timeout=5)
        receiver_stdout = ready_line + receiver_tail
        if receiver.returncode != 0:
            raise RuntimeError(
                f"receiver failed ({receiver.returncode})\n"
                f"stdout:\n{receiver_stdout}\nstderr:\n{receiver_stderr}"
            )
        if "sender_complete generations=2" not in sender.stdout:
            raise RuntimeError(f"missing sender completion: {sender.stdout}")
        if "terminal_ack_datagrams=6" not in sender.stdout:
            raise RuntimeError(f"missing sender terminal ACKs: {sender.stdout}")
        if "receiver_complete generations=2" not in receiver_stdout:
            raise RuntimeError(
                f"missing receiver completion: {receiver_stdout}"
            )
        if "terminal_acknowledged=2" not in receiver_stdout:
            raise RuntimeError(
                f"missing receiver terminal ACK evidence: {receiver_stdout}"
            )
        if not re.search(r"sender_elapsed_ms=[1-9][0-9]*", sender.stdout):
            raise RuntimeError(f"missing sender timing: {sender.stdout}")
        if not re.search(
            r"feedback_rtt_samples=([2-9]|[1-9][0-9]+)(?:\s|$)",
            sender.stdout,
        ):
            raise RuntimeError(f"missing sender-clock RTT samples: {sender.stdout}")
        if not re.search(
            r"terminal_feedback_rtt_samples=2", sender.stdout
        ):
            raise RuntimeError(f"missing terminal RTT samples: {sender.stdout}")
        if "unknown_feedback_echoes=0" not in sender.stdout:
            raise RuntimeError(f"unbound feedback echo: {sender.stdout}")
        if not re.search(
            r"service_elapsed_ms=[1-9][0-9]*", receiver_stdout
        ):
            raise RuntimeError(f"missing receiver timing: {receiver_stdout}")
    finally:
        if receiver.poll() is None:
            receiver.terminate()
            receiver.wait(timeout=3)

    timeout_forward = free_port()
    timeout_feedback = free_port(timeout_forward)
    timed_out = subprocess.run(
        receiver_command(
            executable,
            timeout_forward,
            timeout_feedback,
            trace,
            key_file,
            session_id,
            250,
            2000,
        ),
        capture_output=True,
        text=True,
        timeout=3,
        check=False,
    )
    if timed_out.returncode != 1:
        raise RuntimeError(
            f"startup timeout returned {timed_out.returncode}: "
            f"{timed_out.stdout} {timed_out.stderr}"
        )
    if "receiver_ready" not in timed_out.stdout:
        raise RuntimeError(f"missing timeout readiness: {timed_out.stdout}")
    if "receiver startup timed out" not in timed_out.stderr:
        raise RuntimeError(f"wrong timeout failure: {timed_out.stderr}")

    print("receiver readiness and split timeout evidence passed")


if __name__ == "__main__":
    main()
