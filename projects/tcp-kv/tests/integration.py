#!/usr/bin/env python3
"""Real sockets/processes; all waits bounded and resources unconditionally closed."""
import os
import re
import selectors
import signal
import socket
import struct
import subprocess
import sys
import threading
import time
import unittest

BINARY = os.path.abspath(sys.argv.pop(1))


class Server:
    def __enter__(self):
        self.sockets = []
        self.process = subprocess.Popen(
            [BINARY, "--port", "0"], stdout=subprocess.PIPE, stderr=subprocess.PIPE
        )
        try:
            with selectors.DefaultSelector() as selector:
                selector.register(self.process.stdout, selectors.EVENT_READ)
                if not selector.select(5):
                    raise AssertionError("READY deadline expired")
                # Read bytewise: readline() would block on a malformed partial
                # handshake even after select reported one available byte.
                line = b""
                deadline = time.monotonic() + 5
                while not line.endswith(b"\n") and len(line) < 80:
                    if time.monotonic() >= deadline or not selector.select(0.1):
                        if time.monotonic() >= deadline:
                            raise AssertionError("incomplete READY handshake")
                        continue
                    byte = os.read(self.process.stdout.fileno(), 1)
                    if not byte:
                        raise AssertionError("server exited before READY")
                    line += byte
            match = re.fullmatch(rb"READY ([0-9]+)\n", line)
            if not match or not 0 < int(match[1]) <= 65535:
                raise AssertionError(f"invalid handshake {line!r}")
            self.port = int(match[1])
            return self
        except BaseException:
            self.__exit__(*sys.exc_info())
            raise

    def connect(self, receive_buffer=None):
        peer = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sockets.append(peer)
        peer.settimeout(3)
        if receive_buffer:
            peer.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, receive_buffer)
        peer.connect(("127.0.0.1", self.port))
        return peer

    def stop(self):
        start = time.monotonic()
        self.process.send_signal(signal.SIGTERM)
        code = self.process.wait(timeout=3)
        if code != 0:
            raise AssertionError(f"server exit {code}")
        return time.monotonic() - start

    def __exit__(self, *_):
        for peer in self.sockets:
            peer.close()
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=3)
        stdout, stderr = self.process.communicate(timeout=3)
        if stderr:
            # In particular, sanitizer output is never silently swallowed.
            raise AssertionError(f"server stderr: {stderr.decode(errors='replace')}")
        if self.process.returncode != 0:
            raise AssertionError(f"server exit {self.process.returncode}")


def read_exact(peer, count, timeout=3):
    deadline = time.monotonic() + timeout
    result = bytearray()
    while len(result) < count:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise AssertionError(f"reply deadline: {len(result)}/{count} bytes")
        peer.settimeout(remaining)
        data = peer.recv(count - len(result))
        if not data:
            raise AssertionError(f"early EOF: {len(result)}/{count} bytes")
        result += data
    return bytes(result)


def expect(peer, request, response):
    peer.sendall(request)
    actual = read_exact(peer, len(response))
    if actual != response:
        raise AssertionError(f"expected {response!r}, got {actual!r}")


class Integration(unittest.TestCase):
    def test_protocol_recovery_and_half_close(self):
        with Server() as server:
            peer = server.connect()
            expect(peer, b"SET k two  spaces \nGET k\nDEL k\nGET k\n",
                   b"OK\nVALUE two  spaces \nDELETED\nNOT_FOUND\n")
            expect(peer, b"GET k\r\nget k\nGET k extra\nSET a \xff\nGET k\n",
                   b"ERROR invalid_byte\nERROR unknown_command\nERROR syntax\n"
                   b"ERROR invalid_byte\nNOT_FOUND\n")
            peer.sendall(b"SET after yes\nGET after\nGET unfinished")
            peer.shutdown(socket.SHUT_WR)
            answer = b"OK\nVALUE yes\nERROR incomplete_line\n"
            self.assertEqual(read_exact(peer, len(answer)), answer)
            self.assertEqual(peer.recv(1), b"")
            reconnect = server.connect()
            expect(reconnect, b"GET after\n", b"VALUE yes\n")

    def test_fragmentation_coalescing_and_buffered_fairness(self):
        with Server() as server:
            peer = server.connect()
            frame = b"SET split some value\n"
            for split in range(len(frame) + 1):
                peer.sendall(frame[:split])
                peer.sendall(frame[split:])
                self.assertEqual(read_exact(peer, 3), b"OK\n")
            # More than six command turns, no additional client traffic needed.
            peer.sendall(b"GET split\n" * 111)
            expected = b"VALUE some value\n" * 111
            self.assertEqual(read_exact(peer, len(expected)), expected)
            peer.sendall(b"GET split\n" * 111)
            peer.shutdown(socket.SHUT_WR)
            self.assertEqual(read_exact(peer, len(expected)), expected)
            self.assertEqual(peer.recv(1), b"")
            # A flood cannot prevent a distinct interactive client progressing.
            busy = server.connect()
            interactive = server.connect()
            busy.sendall(b"GET split\n" * 400)
            start = time.monotonic()
            expect(interactive, b"GET split\n", b"VALUE some value\n")
            self.assertLess(time.monotonic() - start, 2)

    def test_key_and_line_boundaries(self):
        with Server() as server:
            peer = server.connect()
            key = b"k" * 64
            value = b"v" * 1024
            expect(peer, b"SET " + key + b" " + value + b"\n", b"OK\n")
            expect(peer, b"GET " + key + b"\n", b"VALUE " + value + b"\n")
            expect(peer, b"GET " + key + b"k\n", b"ERROR key_too_long\n")
            expect(peer, b"SET x " + value + b"v\n", b"ERROR value_too_long\n")
            expect(peer, b"x" * 4095 + b"\n", b"ERROR unknown_command\n")
            expect(peer, b"x" * 4096, b"ERROR line_too_long\n")
            expect(peer, b"discarded\nGET absent\n", b"NOT_FOUND\n")
            # Fill the remainder in small batches, avoiding an unrelated input
            # overflow; the store persists across connections.
            for first in range(0, 1023, 16):
                count = min(16, 1023 - first)
                request = b"".join(f"SET k{i} v\n".encode() for i in range(first, first + count))
                expect(peer, request, b"OK\n" * count)
            expect(peer, b"SET extra v\nSET k0 replaced\nGET k0\nDEL k1\nSET extra v\n",
                   b"ERROR store_full\nOK\nVALUE replaced\nDELETED\nOK\n")

    def test_connection_limit_and_reconnect(self):
        with Server() as server:
            peers = [server.connect() for _ in range(32)]
            # Each acknowledgement proves the socket was accepted, not merely
            # waiting in the listen backlog.
            for peer in peers:
                expect(peer, b"GET x\n", b"NOT_FOUND\n")
            excess = server.connect()
            try:
                self.assertEqual(excess.recv(1), b"")
            except ConnectionResetError:
                pass
            peers[0].shutdown(socket.SHUT_WR)
            self.assertEqual(peers[0].recv(1), b"")
            peers[0].close()
            replacement = server.connect()
            expect(replacement, b"GET x\n", b"NOT_FOUND\n")

    def test_reset_and_clean_empty_eof(self):
        with Server() as server:
            peer = server.connect()
            peer.sendall(b"GET missing\n" * 100)
            peer.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
            peer.close()
            empty = server.connect()
            empty.shutdown(socket.SHUT_WR)
            self.assertEqual(empty.recv(1), b"")
            reconnect = server.connect()
            expect(reconnect, b"SET alive yes\nGET alive\n", b"OK\nVALUE yes\n")

    def test_slow_reader_fairness_and_shutdown(self):
        with Server() as server:
            setup = server.connect()
            expect(setup, b"SET big " + b"z" * 1024 + b"\n", b"OK\n")
            slow = server.connect(receive_buffer=1024)
            stop = threading.Event()
            offered = [0]
            # A bounded helper thread continually offers small requests; it may
            # block when the server closes or applies backpressure. It never
            # needs to read responses to make the slow-reader condition.
            def flood():
                slow.settimeout(0.1)
                deadline = time.monotonic() + 4
                while not stop.is_set() and time.monotonic() < deadline:
                    try:
                        slow.sendall(b"GET big\n" * 16)
                        offered[0] += 16
                    except (OSError, TimeoutError):
                        break
            worker = threading.Thread(target=flood)
            worker.start()
            try:
                interactive = server.connect()
                deadline = time.monotonic() + 0.5
                count = 0
                while time.monotonic() < deadline:
                    expect(interactive, b"GET missing\n", b"NOT_FOUND\n")
                    count += 1
                self.assertGreaterEqual(count, 20)
                self.assertGreaterEqual(offered[0], 16)
                self.assertLess(server.stop(), 3)
            finally:
                stop.set()
                worker.join(timeout=2)
                self.assertFalse(worker.is_alive(), "flood helper failed to terminate")

    def test_signal_shutdown_with_live_idle_clients(self):
        with Server() as server:
            peers = [server.connect() for _ in range(8)]
            for peer in peers:
                expect(peer, b"GET x\n", b"NOT_FOUND\n")
            self.assertLess(server.stop(), 3)

    def test_invalid_cli(self):
        for args in (["--port", "-1"], ["--port", "65536"], ["--port", "12x"],
                     ["--port", ""], ["--unknown"], ["--port"]):
            result = subprocess.run([BINARY, *args], capture_output=True, timeout=5)
            self.assertEqual(result.returncode, 1)
            self.assertIn(b"tcp-kv:", result.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
