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
    if len(sys.argv) != 4:
        raise SystemExit(
            "usage: run_process_emulation.py <emulator> <forward-trace> "
            "<reverse-trace>"
        )

    executable = sys.argv[1]
    trace = sys.argv[2]
    reverse_trace = sys.argv[3]
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
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )

    try:
        sender = subprocess.run(
            [
                executable,
                "sender",
                "127.0.0.1",
                str(forward_port),
                "127.0.0.1",
                str(feedback_port),
                trace,
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
        receiver_stdout, receiver_stderr = receiver.communicate(timeout=10)
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
        if not stale_feedback or int(stale_feedback.group(1)) == 0:
            raise RuntimeError(
                f"reordered feedback was not rejected monotonically: {sender.stdout}"
            )
        delayed = re.search(r"impairment_delayed=(\d+)", sender.stdout)
        reordered = re.search(r"impairment_reordered=(\d+)", sender.stdout)
        if not delayed or not reordered or int(delayed.group(1)) == 0 or int(reordered.group(1)) == 0:
            raise RuntimeError(f"missing timed impairment evidence: {sender.stdout}")
        if receiver_stdout.count("receiver_generation_complete") != 2:
            raise RuntimeError(f"missing per-generation evidence: {receiver_stdout}")
        if "receiver_complete generations=2" not in receiver_stdout:
            raise RuntimeError(f"missing receiver evidence: {receiver_stdout}")
        reverse_delayed = re.search(r"reverse_delayed=(\d+)", receiver_stdout)
        reverse_reordered = re.search(r"reverse_reordered=(\d+)", receiver_stdout)
        reverse_dropped = re.search(r"reverse_dropped=(\d+)", receiver_stdout)
        reverse_duplicated = re.search(r"reverse_duplicated=(\d+)", receiver_stdout)
        reverse_counts = (
            reverse_delayed,
            reverse_reordered,
            reverse_dropped,
            reverse_duplicated,
        )
        if any(value is None or int(value.group(1)) == 0 for value in reverse_counts):
            raise RuntimeError(f"missing reverse impairment evidence: {receiver_stdout}")
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
