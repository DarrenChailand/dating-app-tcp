#!/usr/bin/env python3
"""End-to-end protocol smoke test for NeedLove."""

from __future__ import annotations

import os
import socket
import subprocess
import tempfile
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SERVER = ROOT / "bin" / "needlove-server"


class Peer:
    def __init__(self, port: int) -> None:
        self.socket = socket.create_connection(("127.0.0.1", port), timeout=2)
        self.file = self.socket.makefile("rwb", buffering=0)
        self.expect("WELCOME|")

    def send(self, command: str) -> None:
        self.file.write((command + "\n").encode())

    def read(self) -> str:
        return self.file.readline().decode().rstrip("\r\n")

    def expect(self, prefix: str) -> str:
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            line = self.read()
            if line.startswith(prefix):
                return line
        raise AssertionError(f"did not receive {prefix!r}")

    def close(self) -> None:
        self.file.close()
        self.socket.close()


def main() -> None:
    if not SERVER.exists():
        raise SystemExit("Build first with `make`.")

    port = 42000 + os.getpid() % 1000
    with tempfile.TemporaryDirectory(prefix="needlove-test-") as workdir:
        process = subprocess.Popen(
            [str(SERVER)], cwd=workdir, env={**os.environ},
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
        )
        alice = bob = None
        try:
            # The binary's port is selected at compile time. Rebuild for isolation.
            process.terminate()
            process.wait(timeout=2)
            subprocess.run(
                ["make", "clean", "all", f"PORT={port}"], cwd=ROOT, check=True,
                stdout=subprocess.DEVNULL,
            )
            process = subprocess.Popen(
                [str(SERVER)], cwd=workdir,
                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
            )
            time.sleep(0.1)

            alice, bob = Peer(port), Peer(port)
            alice.send("LOGIN|alice")
            alice.expect("LOGIN_OK")
            alice.expect("PROFILE|")
            alice.expect("NOTIFY_UNREAD|")
            bob.send("LOGIN|bob")
            bob.expect("LOGIN_OK")
            bob.expect("PROFILE|")
            bob.expect("NOTIFY_UNREAD|")

            alice.send("PROFILE_SET|24|woman|man|Systems engineer")
            alice.expect("PROFILE_OK")
            bob.send("PROFILE_SET|25|man|woman|Network engineer")
            bob.expect("PROFILE_OK")

            alice.send("REQUEST_CANDIDATES|10")
            alice.expect("CANDIDATE_BEGIN")
            alice.expect("CANDIDATE|bob|")
            alice.expect("CANDIDATE_END")
            alice.send("SWIPE_BATCH|bob:L")
            alice.expect("SWIPE_OK|1")

            bob.send("REQUEST_CANDIDATES|10")
            bob.expect("CANDIDATE_BEGIN")
            bob.expect("CANDIDATE|alice|")
            bob.expect("CANDIDATE_END")
            bob.send("SWIPE_BATCH|alice:L")
            bob.expect("MATCH_NEW|alice")
            bob.expect("SWIPE_OK|1")
            alice.expect("MATCH_NEW|bob")

            alice.send("CHAT_SEND|bob|hello from the smoke test")
            alice.expect("CHAT_SELF|hello from the smoke test")
            bob.expect("NOTIFY_UNREAD|1")
            print("Smoke test passed: login, profiles, discovery, match, and chat.")
        finally:
            if alice:
                alice.close()
            if bob:
                bob.close()
            process.terminate()
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                process.kill()


if __name__ == "__main__":
    main()
