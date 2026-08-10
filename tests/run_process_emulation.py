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


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: run_process_emulation.py <emulator>")

    executable = sys.argv[1]
    forward_port = free_port()
    feedback_port = free_port(forward_port)
    receiver = subprocess.Popen(
        [executable, "receiver", str(forward_port), str(feedback_port)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )

    try:
        sender = subprocess.run(
            [executable, "sender", str(forward_port), str(feedback_port)],
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
        if "feedback_applied=true" not in sender.stdout:
            raise RuntimeError(f"reverse feedback was not applied: {sender.stdout}")
        if "receiver_complete" not in receiver_stdout:
            raise RuntimeError(f"missing receiver evidence: {receiver_stdout}")
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
