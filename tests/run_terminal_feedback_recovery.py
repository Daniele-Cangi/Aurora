import re
import socket
import subprocess
import sys


def free_port(excluded=None):
    while True:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.bind(("127.0.0.1", 0))
            port = sock.getsockname()[1]
        if port != excluded:
            return port


def require_counter(text, name):
    match = re.search(rf"(?:^|\s){re.escape(name)}=(\d+)(?:\s|$)", text)
    if not match:
        raise RuntimeError(f"missing {name} evidence: {text}")
    return int(match.group(1))


def main():
    if len(sys.argv) != 6:
        raise SystemExit(
            "usage: run_terminal_feedback_recovery.py <emulator> "
            "<forward-trace> <terminal-loss-reverse-trace> <key-file> "
            "<session-id-hex>"
        )

    executable, forward_trace, reverse_trace, key_file, session_id = sys.argv[1:]
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
        ready = receiver.stdout.readline()
        if (
            "receiver_ready" not in ready
            or "protocol_version=3" not in ready
            or "terminal_handshake=authenticated-ack-v1" not in ready
        ):
            raise RuntimeError(f"receiver did not publish v3 readiness: {ready}")
        sender = subprocess.run(
            [
                executable,
                "sender",
                "127.0.0.1",
                str(forward_port),
                "127.0.0.1",
                str(feedback_port),
                forward_trace,
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
        receiver_stdout = ready + receiver_tail
        if receiver.returncode != 0:
            raise RuntimeError(
                f"receiver failed ({receiver.returncode})\n"
                f"stdout:\n{receiver_stdout}\nstderr:\n{receiver_stderr}"
            )
        if (
            "sender_complete generations=2" not in sender.stdout
            or "feedback_applied=2" not in sender.stdout
            or "terminal_handshake=authenticated-ack-v1" not in sender.stdout
        ):
            raise RuntimeError(f"sender completion is incomplete: {sender.stdout}")
        if require_counter(sender.stdout, "terminal_ack_datagrams") != 6:
            raise RuntimeError("sender did not emit three acknowledgements per generation")
        if (
            "receiver_complete generations=2" not in receiver_stdout
            or "terminal_handshake=authenticated-ack-v1" not in receiver_stdout
        ):
            raise RuntimeError(
                f"receiver completion is incomplete: {receiver_stdout}"
            )
        if require_counter(receiver_stdout, "terminal_acknowledged") != 2:
            raise RuntimeError("receiver did not authenticate every terminal ACK")
        if require_counter(receiver_stdout, "terminal_feedback_retry_rounds") < 1:
            raise RuntimeError(
                "the loss trace did not exercise terminal feedback recovery"
            )
        if require_counter(receiver_stdout, "reverse_dropped") < 3:
            raise RuntimeError("the loss trace did not drop the terminal copies")
        require_counter(receiver_stdout, "terminal_ack_wait_ms")
        print("authenticated terminal feedback recovery passed")
    finally:
        if receiver.poll() is None:
            receiver.terminate()
            receiver.wait(timeout=3)


if __name__ == "__main__":
    main()
