#!/usr/bin/env python3
"""Tests of the reference server (tools/reference_server.py), from a client written here in Python: every message
type, the refusals and their codes, which ones close the connection, results equal to a local run, PONG only after
HELLO, and answers in order, late ones too; and version 2: the version rule (a version 1 server refusing a version 2
HELLO with ERROR 6 framed in version 1), NOTICE, and FIND and FOUND on UDP from the Remote IP alone.

    python3 tools/test_reference_server.py <folder holding tiny.onnx and other/tiny.onnx (tests/make_test_model.py)>
"""

import hashlib
import json
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest

import numpy as np
import onnxruntime as ort

HERE = os.path.dirname(os.path.abspath(__file__))
SERVER = os.path.join(HERE, "reference_server.py")
MODELS = None

HEADER = struct.Struct("<4sHHI")


def pstr(s):
    b = s.encode("utf-8")
    return struct.pack("<H", len(b)) + b


def frame(kind, payload, magic=b"BFAR", version=1, length=None):
    return HEADER.pack(magic, version, kind, len(payload) if length is None else length) + payload


def digest(path):
    with open(path, "rb") as f:
        data = f.read()
    return len(data), hashlib.sha256(data).digest()


def hello(key="", models=(), version=1):
    p = pstr("test client") + pstr(key) + pstr("1.24.4") + struct.pack("<H", len(models))
    for name, size, sha, overrides in models:
        p += pstr(name) + struct.pack("<Q", size) + sha + struct.pack("<H", len(overrides))
        for dim, value in overrides:
            p += pstr(dim) + struct.pack("<q", value)
    return frame(1, p, version=version)


def notice(model, reason, detail, lines, version=2):
    return frame(10, pstr(model) + pstr(reason) + pstr(detail) + struct.pack("<I", lines), version=version)


def tensor(name, array):
    codes = {np.dtype("float32"): 1, np.dtype("int32"): 6, np.dtype("int64"): 7, np.dtype("float16"): 10, np.dtype("float64"): 11}
    data = np.ascontiguousarray(array).tobytes()
    return (pstr(name) + struct.pack("<BB", codes[array.dtype], array.ndim) + b"".join(struct.pack("<q", d) for d in array.shape)
            + struct.pack("<I", len(data)) + data)


def run(request_id, model, inputs, outputs, version=1):
    p = struct.pack("<I", request_id) + pstr(model) + struct.pack("<H", len(inputs))
    for name, array in inputs:
        p += tensor(name, array)
    p += struct.pack("<H", len(outputs))
    for name in outputs:
        p += pstr(name)
    return frame(3, p, version=version)


class Conn:
    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=30)

    def send(self, data):
        self.s.sendall(data)

    def recv_exact(self, n):
        b = b""
        while len(b) < n:
            chunk = self.s.recv(n - len(b))
            if not chunk:
                return None
            b += chunk
        return b

    def answer(self):
        """(type, payload) of the next frame; its version is kept in last_version."""
        head = self.recv_exact(12)
        if head is None:
            return None
        magic, version, kind, length = HEADER.unpack(head)
        assert magic == b"BFAR"
        self.last_version = version
        return kind, self.recv_exact(length) if length else b""

    def closed(self):
        """Whether the server closed the connection: an end of stream or a reset within 5 s; still open after that."""
        self.s.settimeout(5)
        try:
            return self.s.recv(1) == b""
        except socket.timeout:
            return False
        except OSError:
            return True
        finally:
            self.s.settimeout(30)

    def close(self):
        self.s.close()


def error_code(payload):
    request_id, code = struct.unpack_from("<IH", payload)
    return request_id, code


def tiny_inputs(seed=0):
    rng = np.random.default_rng(seed)
    return [
        ("x", rng.standard_normal((2, 8)).astype(np.float32)),
        ("id", np.array([5], dtype=np.int64)),
        ("h", rng.standard_normal((2, 4)).astype(np.float16)),
        ("d", np.array([1.5, -2.0], dtype=np.float64)),
        ("i", np.array([1, 2, 3], dtype=np.int32)),
    ]


def parse_run_ok(payload):
    request_id, micros, count = struct.unpack_from("<IIH", payload)
    at = 10
    out = []
    dtypes = {1: "<f4", 6: "<i4", 7: "<i8", 10: "<f2", 11: "<f8"}
    for _ in range(count):
        (n,) = struct.unpack_from("<H", payload, at)
        name = payload[at + 2:at + 2 + n].decode()
        at += 2 + n
        code, rank = struct.unpack_from("<BB", payload, at)
        at += 2
        dims = struct.unpack_from("<%dq" % rank, payload, at)
        at += 8 * rank
        (length,) = struct.unpack_from("<I", payload, at)
        at += 4
        out.append((name, np.frombuffer(payload[at:at + length], dtype=dtypes[code]).reshape(dims)))
        at += length
    assert at == len(payload)
    return request_id, micros, out


def start_server(*extra):
    proc = subprocess.Popen([sys.executable, "-I", SERVER, "--models", MODELS, "--port", "0", "--quiet", "--key", "sesame", *extra],
                            stdout=subprocess.PIPE, text=True)
    line = proc.stdout.readline()
    assert line.startswith("LISTENING "), line
    return proc, int(line.split()[1])


def find(version=2, kind=8, client="BFARemoteBridge 1.3.0 (answered 0, fallbacks 0, timeouts 0)", extra=b""):
    """A FIND datagram (or a wrong one, for the tests)."""
    payload = pstr(client) + extra
    return HEADER.pack(b"BFAR", version, kind, len(payload)) + payload


class Udp:
    """A UDP socket of the test's own: FIND out, FOUND back."""

    def __init__(self):
        self.s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.s.bind(("127.0.0.1", 0))

    def ask(self, port, datagram, wait=0.5):
        """The answer to one datagram sent to port, or None when none came within wait seconds."""
        self.s.sendto(datagram, ("127.0.0.1", port))
        self.s.settimeout(wait)
        try:
            data, sender = self.s.recvfrom(2048)
        except (socket.timeout, ConnectionError):
            return None
        return data, sender

    def close(self):
        self.s.close()


class ReferenceServerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.notices = tempfile.NamedTemporaryFile("w", suffix=".jsonl", delete=False).name
        cls.proc, cls.port = start_server("--notice-log", cls.notices)
        cls.tiny = os.path.join(MODELS, "tiny.onnx")
        size, sha = digest(cls.tiny)
        cls.entry = ("tiny.onnx", size, sha, [("batch", 2)])

    @classmethod
    def tearDownClass(cls):
        cls.proc.terminate()
        cls.proc.wait()
        cls.proc.stdout.close()
        os.unlink(cls.notices)

    def logged_notices(self):
        with open(self.notices, encoding="utf-8") as f:
            return [json.loads(line) for line in f if line.strip()]

    def connect(self, port=None):
        c = Conn(port or self.port)
        self.addCleanup(c.close)
        return c

    def other_server(self, *extra):
        proc, port = start_server(*extra)

        def stop():
            proc.terminate()
            proc.wait()
            proc.stdout.close()

        self.addCleanup(stop)
        return port

    def test_hello_statuses(self):
        c = self.connect()
        other_size, other_sha = digest(os.path.join(MODELS, "other", "tiny.onnx"))
        c.send(hello("sesame", [self.entry, ("tiny.onnx", other_size, other_sha, [("batch", 2)]), ("missing.onnx", 1, b"\0" * 32, [])]))
        kind, payload = c.answer()
        self.assertEqual(kind, 2)
        at = 0
        names = []
        for _ in range(2):
            (n,) = struct.unpack_from("<H", payload, at)
            at += 2 + n
        (count,) = struct.unpack_from("<H", payload, at)
        at += 2
        statuses = []
        for _ in range(count):
            (n,) = struct.unpack_from("<H", payload, at)
            names.append(payload[at + 2:at + 2 + n].decode())
            at += 2 + n
            statuses.append(payload[at])
            at += 1
            (n,) = struct.unpack_from("<H", payload, at)
            at += 2 + n
        self.assertEqual(names, ["tiny.onnx", "tiny.onnx", "missing.onnx"])
        self.assertEqual(statuses, [0, 2, 1])

    def test_run_matches_local_and_ping(self):
        c = self.connect()
        c.send(hello("sesame", [self.entry]))
        self.assertEqual(c.answer()[0], 2)
        c.send(frame(6, b""))
        self.assertEqual(c.answer(), (6 + 1, b""))
        inputs = tiny_inputs(3)
        names = ["y", "z", "hh", "dd", "ii"]
        c.send(run(77, "tiny.onnx", inputs, names))
        kind, payload = c.answer()
        self.assertEqual(kind, 4)
        request_id, micros, out = parse_run_ok(payload)
        self.assertEqual(request_id, 77)
        options = ort.SessionOptions()
        options.add_free_dimension_override_by_name("batch", 2)
        options.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_EXTENDED
        local = ort.InferenceSession(self.tiny, sess_options=options, providers=["CPUExecutionProvider"]).run(names, dict(inputs))
        self.assertEqual([n for n, _ in out], names)
        for (_, got), want in zip(out, local):
            self.assertEqual(got.dtype, want.dtype)
            self.assertEqual(got.tobytes(), want.tobytes())
        # the warm-up run BFA makes: zeros
        zeros = [(n, np.zeros_like(a)) for n, a in inputs]
        c.send(run(78, "tiny.onnx", zeros, ["y"]))
        self.assertEqual(c.answer()[0], 4)

    def test_refusals_that_close(self):
        cases = [
            (frame(1, b"", magic=b"XXXX"), 1),       # bad magic
            (frame(1, b"", version=3), 6),           # protocol version 3
            (frame(1, b"", version=0), 6),           # protocol version 0
            (hello("wrong", [self.entry]), 2),       # wrong key
            (hello("sesame", [("../tiny.onnx", 1, b"\0" * 32, [])]), 1),  # a name with a path
            (hello("sesame", [("models\\tiny.onnx", 1, b"\0" * 32, [])]), 1),  # a backslash
            (hello("sesame", [("C:tiny.onnx", 1, b"\0" * 32, [])]), 1),  # a drive letter's colon
            (hello("sesame", [("tiny..onnx", 1, b"\0" * 32, [])]), 1),  # ".." anywhere
            (hello("sesame", [("..", 1, b"\0" * 32, [])]), 1),
            (hello("sesame", [("x" * 300 + ".onnx", 1, b"\0" * 32, [])]), 1),  # over-long
            (run(1, "tiny.onnx", [], []), 1),        # RUN before HELLO
            (frame(6, b""), 1),                      # PING before HELLO: no PONG
            (frame(1, b"\x01"), 1),                  # truncated HELLO
        ]
        for data, code in cases:
            c = self.connect()
            c.send(data)
            kind, payload = c.answer()
            self.assertEqual(kind, 5)
            self.assertEqual(error_code(payload)[1], code)
            self.assertTrue(c.closed())

    def test_refusals_that_keep_the_connection(self):
        c = self.connect()
        c.send(hello("sesame", [self.entry]))
        self.assertEqual(c.answer()[0], 2)
        c.send(run(5, "other.onnx", [], ["y"]))  # unknown model
        kind, payload = c.answer()
        self.assertEqual((kind, error_code(payload)), (5, (5, 3)))
        c.send(run(6, "tiny.onnx", [("x", np.zeros((1, 2), np.float32))], ["y"]))  # inference fails
        kind, payload = c.answer()
        self.assertEqual((kind, error_code(payload)), (5, (6, 4)))
        c.send(frame(6, b""))
        self.assertEqual(c.answer()[0], 7)  # still open
        # names in a RUN: a path, an over-long model name, an over-long tensor name
        for model, inputs in [("..\\tiny.onnx", []), ("y" * 300, []), ("tiny.onnx", [("z" * 300, np.zeros(1, np.float32))])]:
            c.send(run(9, model, inputs, ["y"]))
            kind, payload = c.answer()
            self.assertEqual((kind, error_code(payload)), (5, (9, 1)))
            self.assertTrue(c.closed())
            c = self.connect()
            c.send(hello("sesame", [self.entry]))
            self.assertEqual(c.answer()[0], 2)

    def test_answers_in_order(self):
        # requests sent back to back are answered in the order sent, PONG among them
        c = self.connect()
        c.send(hello("sesame", [self.entry]) + run(11, "tiny.onnx", tiny_inputs(1), ["y"]) + frame(6, b"") +
               run(12, "tiny.onnx", tiny_inputs(2), ["y"]) + run(13, "missing.onnx", [], ["y"]))
        self.assertEqual(c.answer()[0], 2)
        kind, payload = c.answer()
        self.assertEqual((kind, parse_run_ok(payload)[0]), (4, 11))
        self.assertEqual(c.answer(), (7, b""))
        kind, payload = c.answer()
        self.assertEqual((kind, parse_run_ok(payload)[0]), (4, 12))
        kind, payload = c.answer()
        self.assertEqual((kind, error_code(payload)), (5, (13, 3)))

    def test_a_late_answer_comes_before_the_next(self):
        # the first RUN is slow: its answer, late for a client that gave up, still comes first
        port = self.other_server("--delay-ms", "400", "--fault-count", "1")
        c = self.connect(port)
        c.send(hello("sesame", [self.entry]))
        self.assertEqual(c.answer()[0], 2)
        start = time.monotonic()
        c.send(run(21, "tiny.onnx", tiny_inputs(1), ["y"]))
        c.send(run(22, "tiny.onnx", tiny_inputs(2), ["y"]))
        ids = [parse_run_ok(c.answer()[1])[0] for _ in range(2)]
        self.assertEqual(ids, [21, 22])
        self.assertGreaterEqual(time.monotonic() - start, 0.39)
        c.send(run(23, "tiny.onnx", tiny_inputs(3), ["y"]))  # past --fault-count: at once
        start = time.monotonic()
        self.assertEqual(parse_run_ok(c.answer()[1])[0], 23)
        self.assertLess(time.monotonic() - start, 0.35)

    def test_error_codes_that_close(self):
        # after ERROR 1, 2, 5, 6 or 7 the connection is closed; after 3 or 4 it stays open
        for code in (1, 2, 3, 4, 5, 6, 7):
            port = self.other_server("--run-error", str(code))
            c = self.connect(port)
            c.send(hello("sesame", [self.entry]))
            self.assertEqual(c.answer()[0], 2)
            c.send(run(31, "tiny.onnx", tiny_inputs(1), ["y"]))
            kind, payload = c.answer()
            self.assertEqual((kind, error_code(payload)), (5, (31, code)), code)
            if code in (1, 2, 5, 6, 7):
                self.assertTrue(c.closed(), code)
            else:
                c.send(frame(6, b""))
                self.assertEqual(c.answer(), (7, b""), code)

    def test_oversize_frame_is_refused_and_closed(self):
        c = self.connect()
        c.send(hello("sesame", [self.entry]))
        self.assertEqual(c.answer()[0], 2)
        c.send(frame(3, b"", length=64 * 1024 * 1024 + 1)[:12])  # the header alone: the payload is never read
        kind, payload = c.answer()
        self.assertEqual((kind, error_code(payload)[1]), (5, 5))
        self.assertTrue(c.closed())

    # ---- version 2

    def test_each_connection_is_answered_in_its_own_version(self):
        for version in (1, 2):
            c = self.connect()
            c.send(hello("sesame", [self.entry], version=version))
            self.assertEqual(c.answer()[0], 2)
            self.assertEqual(c.last_version, version)
            c.send(frame(6, b"", version=version))
            self.assertEqual(c.answer(), (7, b""))
            self.assertEqual(c.last_version, version)
            c.send(run(41, "tiny.onnx", tiny_inputs(1), ["y"], version=version))
            kind, payload = c.answer()
            self.assertEqual((kind, parse_run_ok(payload)[0], c.last_version), (4, 41, version))
            c.send(run(42, "missing.onnx", [], ["y"], version=version))
            kind, payload = c.answer()
            self.assertEqual((kind, error_code(payload), c.last_version), (5, (42, 3), version))

    def test_a_frame_of_another_version_is_refused(self):
        # on an open connection, a frame of another version: ERROR 6 framed in the connection's, then closed
        for opened, other in ((2, 1), (1, 2)):
            c = self.connect()
            c.send(hello("sesame", [self.entry], version=opened))
            self.assertEqual(c.answer()[0], 2)
            c.send(frame(6, b"", version=other))
            kind, payload = c.answer()
            self.assertEqual((kind, error_code(payload)[1], c.last_version), (5, 6, opened))
            self.assertTrue(c.closed())
        # a version this server doesn't speak, opening: ERROR 6 framed in the newest it speaks
        c = self.connect()
        c.send(hello("sesame", [self.entry], version=3))
        kind, payload = c.answer()
        self.assertEqual((kind, error_code(payload), c.last_version), (5, (0, 6), 2))
        self.assertTrue(c.closed())

    def test_a_version_1_server_refuses_a_version_2_hello(self):
        port = self.other_server("--v1-only")
        c = self.connect(port)
        c.send(hello("sesame", [self.entry], version=2))
        kind, payload = c.answer()
        self.assertEqual((kind, error_code(payload), c.last_version), (5, (0, 6), 1))
        self.assertTrue(c.closed())
        # the bridge then opens in version 1, which is answered
        c = self.connect(port)
        c.send(hello("sesame", [self.entry], version=1))
        self.assertEqual((c.answer()[0], c.last_version), (2, 1))
        # and NOTICE, version 2's, isn't a version 1 message
        c.send(notice("tiny.onnx", "timeout", "no answer within 1000 ms", 1, version=1))
        kind, payload = c.answer()
        self.assertEqual((kind, error_code(payload)[1]), (5, 1))
        self.assertTrue(c.closed())

    def test_notice_is_one_way_and_logged(self):
        before = len(self.logged_notices())
        c = self.connect()
        c.send(hello("sesame", [self.entry], version=2))
        self.assertEqual(c.answer()[0], 2)
        c.send(run(51, "tiny.onnx", tiny_inputs(1), ["y"], version=2))
        self.assertEqual(c.answer()[0], 4)
        # NOTICE between a RUN's answer and the next request: nothing comes back, so the next answer is the PING's
        c.send(notice("tiny.onnx", "error", "ERROR 4, inference failed: a test", 3))
        c.send(frame(6, b"", version=2))
        self.assertEqual(c.answer(), (7, b""))
        # one it can't read is ignored, unanswered, and the connection stays
        c.send(frame(10, pstr("tiny.onnx") + pstr("timeout"), version=2))
        c.send(notice("../tiny.onnx", "timeout", "", 1))
        c.send(frame(6, b"", version=2))
        self.assertEqual(c.answer(), (7, b""))
        got = self.logged_notices()[before:]
        self.assertEqual(got, [{"client": "test client", "model": "tiny.onnx", "reason": "error",
                                "detail": "ERROR 4, inference failed: a test", "lines": 3}])

    def test_notice_needs_hello_and_version_2(self):
        for data in (notice("tiny.onnx", "timeout", "", 1),               # before HELLO
                     hello("sesame", [self.entry], version=1) + notice("tiny.onnx", "timeout", "", 1, version=1)):
            c = self.connect()
            c.send(data)
            kind, payload = c.answer()
            if kind == 2:
                kind, payload = c.answer()
            self.assertEqual((kind, error_code(payload)[1]), (5, 1))
            self.assertTrue(c.closed())
        # FIND and FOUND are datagrams: on TCP, malformed
        c = self.connect()
        c.send(hello("sesame", [self.entry], version=2) + frame(8, pstr("x"), version=2))
        self.assertEqual(c.answer()[0], 2)
        kind, payload = c.answer()
        self.assertEqual((kind, error_code(payload)[1]), (5, 1))

    def test_find_is_answered_from_the_remote_ip_alone(self):
        port = self.other_server("--remote-ip", "127.0.0.1")
        u = Udp()
        self.addCleanup(u.close)
        got = u.ask(port, find())
        self.assertIsNotNone(got)
        data, sender = got
        self.assertEqual(sender, ("127.0.0.1", port))
        magic, version, kind, length = HEADER.unpack_from(data)
        self.assertEqual((magic, version, kind, length), (b"BFAR", 2, 9, len(data) - 12))
        (n,) = struct.unpack_from("<H", data, 12)
        self.assertEqual(14 + n, len(data))  # FOUND: str server, and no port
        self.assertTrue(data[14:].decode().startswith("BFA Remote Bridge reference server"))
        # at most one FOUND a second
        self.assertIsNone(u.ask(port, find(), wait=0.3))
        time.sleep(1.0)
        self.assertIsNotNone(u.ask(port, find()))
        # anything but a well-formed version 2 FIND is ignored
        time.sleep(1.1)
        for wrong in (find(version=1), find(kind=9), find(extra=b"x"), find()[:-1], b"BFAR", b"XXXX" + find()[4:]):
            self.assertIsNone(u.ask(port, wrong, wait=0.3))
        self.assertIsNotNone(u.ask(port, find()))

    def test_find_from_another_address_or_to_a_version_1_server_goes_unanswered(self):
        u = Udp()
        self.addCleanup(u.close)
        self.assertIsNone(u.ask(self.other_server("--remote-ip", "127.0.0.2"), find()))
        self.assertIsNone(u.ask(self.other_server("--remote-ip", "127.0.0.1", "--v1-only"), find()))
        self.assertIsNone(u.ask(self.port, find()))  # no Remote IP: no FIND


if __name__ == "__main__":
    MODELS = sys.argv.pop(1)
    unittest.main(verbosity=2)
