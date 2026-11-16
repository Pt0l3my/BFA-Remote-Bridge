#!/usr/bin/env python3
"""Tests of the reference server (tools/reference_server.py), from a client written here in Python: every message
type, the refusals and their codes, which ones close the connection, results equal to a local run, PONG only after
HELLO, and answers in order, late ones too; version 2: the version rule (a version 1 server refusing a version 2
HELLO with ERROR 6 framed in version 1), NOTICE, and FIND and FOUND on UDP from the Remote IP alone; and version 3 (the
agreement, docs/AGREEMENT-1.7.md): HELLO_OK's held, held runs against carried ones byte for byte, ERROR 8 in its three
cases, the four refusals, the cap of 8, the states freed, a released version 2 panel's answer, and the test options
that drop held state.

    python3 tools/test_reference_server.py <folder holding tiny.onnx, other/tiny.onnx and stateful.onnx
                                            (tests/make_test_model.py)>
"""

import hashlib
import json
import os
import shutil
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


def run(request_id, model, inputs, outputs, version=1, stream=0, mode=0):
    p = struct.pack("<I", request_id)
    if version >= 3:
        p += struct.pack("<QB", stream, mode)
    p += pstr(model) + struct.pack("<H", len(inputs))
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


def parse_hello_ok(payload, version):
    """[(name, status, detail, held)], held None below version 3; the payload read to its end."""
    at = 0
    for _ in range(2):
        (n,) = struct.unpack_from("<H", payload, at)
        at += 2 + n
    (count,) = struct.unpack_from("<H", payload, at)
    at += 2
    rows = []
    for _ in range(count):
        (n,) = struct.unpack_from("<H", payload, at)
        name = payload[at + 2:at + 2 + n].decode()
        at += 2 + n
        status = payload[at]
        (n,) = struct.unpack_from("<H", payload, at + 1)
        detail = payload[at + 3:at + 3 + n].decode()
        at += 3 + n
        held = None
        if version >= 3:
            held = payload[at]
            at += 1
        rows.append((name, status, detail, held))
    assert at == len(payload)
    return rows


STATE = (2, 1, 2, 8, 3)  # stateful.onnx's state; F = 4 frames a run, W = 8


def window(seed):
    """One window of stateful.onnx's inputs but its state."""
    rng = np.random.default_rng(seed)
    return [("mel", rng.standard_normal((1, 4, 8)).astype(np.float32)), ("identity", np.array([seed % 2], dtype=np.int64)),
            ("emotion", rng.standard_normal((1, 4, 2)).astype(np.float32))]


def line_frames(count):
    """BFA's past_frames over a line: 0, 4, 8, 8, ..."""
    frames, out = 0, []
    for _ in range(count):
        out.append(frames)
        frames = min(frames + 4, 8)
    return out


def local_line(path, seeds):
    """A line of stateful.onnx run here, each run carrying the last one's state: its motions."""
    session = ort.InferenceSession(path, providers=["CPUExecutionProvider"])
    k = np.zeros(STATE, np.float32)
    v = np.zeros(STATE, np.float32)
    motions = []
    for seed, frames in zip(seeds, line_frames(len(seeds))):
        feeds = dict(window(seed))
        feeds.update(past_k=k, past_v=v, past_frames=np.array([frames], dtype=np.int64))
        motion, k, v = session.run(["normalized_motion", "next_k", "next_v"], feeds)
        motions.append(motion)
    return motions


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
        cls.stateful = os.path.join(MODELS, "stateful.onnx")
        size, sha = digest(cls.stateful)
        cls.held_entry = ("stateful.onnx", size, sha, [])

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
            (frame(1, b"", version=4), 6),           # protocol version 4
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
        for version in (1, 2, 3):
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
        for opened, other in ((2, 1), (1, 2), (3, 2), (2, 3)):
            c = self.connect()
            c.send(hello("sesame", [self.entry], version=opened))
            self.assertEqual(c.answer()[0], 2)
            c.send(frame(6, b"", version=other))
            kind, payload = c.answer()
            self.assertEqual((kind, error_code(payload)[1], c.last_version), (5, 6, opened))
            self.assertTrue(c.closed())
        # a version this server doesn't speak, opening: ERROR 6 framed in the newest it speaks
        c = self.connect()
        c.send(hello("sesame", [self.entry], version=4))
        kind, payload = c.answer()
        self.assertEqual((kind, error_code(payload), c.last_version), (5, (0, 6), 3))
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

    def test_a_p6_server_closes_with_the_hello_unread(self):
        # p6 and earlier read only the header of a version 2 HELLO, answer ERROR 6 framed in version 1 and close with
        # the payload unread: the client may read the ERROR, or find the connection reset (the race the bridge's
        # no-frame rule covers). Either way the connection ends, and version 1 is then answered.
        port = self.other_server("--p6")
        c = self.connect(port)
        c.send(hello("sesame", [self.entry], version=2))
        time.sleep(0.2)
        try:
            got = c.answer()
        except ConnectionError:
            got = "reset"
        if got not in ("reset", None):
            kind, payload = got
            self.assertEqual((kind, error_code(payload), c.last_version), (5, (0, 6), 1))
            self.assertTrue(c.closed())
        c = self.connect(port)
        c.send(hello("sesame", [self.entry], version=1))
        self.assertEqual((c.answer()[0], c.last_version), (2, 1))

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
        # version 3 changes nothing on UDP: a FIND framed in 3 is ignored too
        for wrong in (find(version=1), find(version=3), find(kind=9), find(extra=b"x"), find()[:-1], b"BFAR", b"XXXX" + find()[4:]):
            self.assertIsNone(u.ask(port, wrong, wait=0.3))
        self.assertIsNotNone(u.ask(port, find()))

    def test_find_from_another_address_or_to_a_version_1_server_goes_unanswered(self):
        u = Udp()
        self.addCleanup(u.close)
        self.assertIsNone(u.ask(self.other_server("--remote-ip", "127.0.0.2"), find()))
        self.assertIsNone(u.ask(self.other_server("--remote-ip", "127.0.0.1", "--v1-only"), find()))
        self.assertIsNone(u.ask(self.port, find()))  # no Remote IP: no FIND

    def test_the_sizes_are_filled_in_when_a_hello_lists_none(self):
        port = self.other_server("--fill-sizes", self.entry[2].hex() + ":batch=2")
        details = []
        for overrides in ([], [("batch", 2)]):
            c = self.connect(port)
            c.send(hello("sesame", [(self.entry[0], self.entry[1], self.entry[2], overrides)], version=2))
            kind, payload = c.answer()
            self.assertEqual(kind, 2)
            at = 0
            for _ in range(2):
                (n,) = struct.unpack_from("<H", payload, at)
                at += 2 + n
            at += 2
            (n,) = struct.unpack_from("<H", payload, at)
            at += 2 + n
            status = payload[at]
            (n,) = struct.unpack_from("<H", payload, at + 1)
            details.append((status, payload[at + 3:at + 3 + n].decode()))
            c.send(run(61, "tiny.onnx", tiny_inputs(1), ["y"], version=2))  # batch 2 either way
            self.assertEqual(c.answer()[0], 4)
        self.assertEqual(details, [(0, "built with BFA 4.3.2.3's sizes"), (0, "")])

    def test_hello_log_records_the_models_and_overrides_as_listed(self):
        path = tempfile.NamedTemporaryFile("w", suffix=".jsonl", delete=False).name
        self.addCleanup(os.unlink, path)
        port = self.other_server("--hello-log", path)
        c = self.connect(port)
        c.send(hello("sesame", [self.entry], version=2))
        self.assertEqual(c.answer()[0], 2)
        c.send(hello("sesame", [(self.entry[0], self.entry[1], self.entry[2], [])], version=2))
        self.assertEqual(c.answer()[0], 2)
        with open(path, encoding="utf-8") as f:
            logged = [json.loads(line) for line in f if line.strip()]
        self.assertEqual([h["models"] for h in logged],
                         [[{"name": "tiny.onnx", "overrides": [["batch", 2]]}], [{"name": "tiny.onnx", "overrides": []}]])
        self.assertEqual([(h["client"], h["version"]) for h in logged], [("test client", 2)] * 2)

    # ---- version 3

    def held_conn(self, port=None, models=None):
        """A version 3 connection whose HELLO listed tiny.onnx and stateful.onnx (or models)."""
        c = self.connect(port)
        c.send(hello("sesame", models or [self.entry, self.held_entry], version=3))
        kind, payload = c.answer()
        self.assertEqual((kind, c.last_version), (2, 3))
        return c, parse_hello_ok(payload, 3)

    def held_run(self, c, request_id, stream, mode, seed, frames, model="stateful.onnx", outputs=("normalized_motion",), extra=()):
        inputs = window(seed) + [("past_frames", np.array([frames], dtype=np.int64))] + list(extra)
        c.send(run(request_id, model, inputs, list(outputs), version=3, stream=stream, mode=mode))
        return c.answer()

    def held_line(self, c, stream, seeds, first_id=100):
        """A line in held mode: a start, then continues; each answer's motion."""
        motions = []
        for i, (seed, frames) in enumerate(zip(seeds, line_frames(len(seeds)))):
            kind, payload = self.held_run(c, first_id + i, stream, 1 if frames == 0 else 2, seed, frames)
            self.assertEqual(kind, 4, payload)
            request_id, _, out = parse_run_ok(payload)
            self.assertEqual((request_id, [n for n, _ in out]), (first_id + i, ["normalized_motion"]))
            motions.append(out[0][1])
        return motions

    def test_hello_ok_rows_carry_held_in_version_3_only(self):
        c, rows = self.held_conn()
        self.assertEqual([(n, st, h) for n, st, _, h in rows], [("tiny.onnx", 0, 0), ("stateful.onnx", 0, 1)])
        # a model not ready is held 0
        c, rows = self.held_conn(models=[("missing.onnx", 1, b"\0" * 32, []), self.held_entry])
        self.assertEqual([(st, h) for _, st, _, h in rows], [(1, 0), (0, 1)])
        # version 2's rows have no held byte
        for version in (1, 2):
            c = self.connect()
            c.send(hello("sesame", [self.entry, self.held_entry], version=version))
            kind, payload = c.answer()
            self.assertEqual([(n, st) for n, st, _, _ in parse_hello_ok(payload, version)], [("tiny.onnx", 0), ("stateful.onnx", 0)])
        # the panel's Keep line state off: held 0 for every model
        c, rows = self.held_conn(self.other_server("--no-held"))
        self.assertEqual([h for _, _, _, h in rows], [0, 0])

    def test_held_runs_answer_as_carried_runs_byte_for_byte(self):
        seeds = [11, 12, 13, 14, 15, 16]
        local = local_line(self.stateful, seeds)
        c, _ = self.held_conn()
        held = self.held_line(c, 7, seeds)
        for got, want in zip(held, local):
            self.assertEqual(got.tobytes(), want.tobytes())
        # the same line carried, mode 0 in version 3 and a version 2 RUN: the state travels both ways
        for version in (3, 2):
            c = self.connect()
            c.send(hello("sesame", [self.held_entry], version=version))
            self.assertEqual(c.answer()[0], 2)
            k = np.zeros(STATE, np.float32)
            v = np.zeros(STATE, np.float32)
            for i, (seed, frames) in enumerate(zip(seeds, line_frames(len(seeds)))):
                inputs = window(seed) + [("past_k", k), ("past_v", v), ("past_frames", np.array([frames], dtype=np.int64))]
                c.send(run(200 + i, "stateful.onnx", inputs, ["normalized_motion", "next_k", "next_v"], version=version))
                kind, payload = c.answer()
                self.assertEqual(kind, 4)
                out = dict(parse_run_ok(payload)[2])
                self.assertEqual(out["normalized_motion"].tobytes(), local[i].tobytes())
                k, v = out["next_k"], out["next_v"]

    def test_error_8_in_its_three_cases_keeps_the_connection(self):
        folder = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, folder)
        shutil.copy(self.stateful, os.path.join(folder, "stateful.onnx"))
        shutil.copy(self.stateful, os.path.join(folder, "second.onnx"))
        port = self.other_server("--models", folder)
        second = ("second.onnx",) + self.held_entry[1:]
        c, rows = self.held_conn(port, [self.held_entry, second])
        self.assertEqual([h for _, _, _, h in rows], [1, 1])
        # no state for the stream
        kind, payload = self.held_run(c, 1, 5, 2, 1, 4)
        self.assertEqual((kind, error_code(payload)), (5, (1, 8)))
        # another model's state
        self.assertEqual(self.held_run(c, 2, 5, 1, 1, 0)[0], 4)
        kind, payload = self.held_run(c, 3, 5, 2, 2, 4, model="second.onnx")
        self.assertEqual((kind, error_code(payload)), (5, (3, 8)))
        # another count: 8 where 4 is due
        kind, payload = self.held_run(c, 4, 5, 2, 2, 8)
        self.assertEqual((kind, error_code(payload)), (5, (4, 8)))
        # each left the state as it was, and the connection open: the line goes on
        self.assertEqual(self.held_run(c, 5, 5, 2, 2, 4)[0], 4)
        # a run that fails changes nothing either
        kind, payload = self.held_run(c, 6, 5, 2, 3, 8, extra=[("mel_extra", np.zeros(1, np.float32))])
        self.assertEqual((kind, error_code(payload)[1]), (5, 4))
        self.assertEqual(self.held_run(c, 7, 5, 2, 3, 8)[0], 4)
        c.send(frame(6, b"", version=3))
        self.assertEqual(c.answer(), (7, b""))
        # held per connection: another connection holds nothing for the stream
        d, _ = self.held_conn(port, [self.held_entry])
        kind, payload = self.held_run(d, 8, 5, 2, 4, 8)
        self.assertEqual((kind, error_code(payload)), (5, (8, 8)))

    def test_the_four_refusals_are_error_1_and_close_the_connection(self):
        frames0 = [("past_frames", np.array([0], dtype=np.int64))]
        # a mode other than 0, 1 or 2; a held mode for a model marked held 0
        for model, inputs, outputs, mode in (("stateful.onnx", window(1) + frames0, ["normalized_motion"], 3),
                                             ("tiny.onnx", tiny_inputs(1), ["y"], 1)):
            c, _ = self.held_conn()
            c.send(run(9, model, inputs, outputs, version=3, stream=1, mode=mode))
            kind, payload = c.answer()
            self.assertEqual((kind, error_code(payload)), (5, (9, 1)), (model, mode))
            self.assertTrue(c.closed())
        # mode 1 with past_frames other than 0 (or none), mode 2 with 0; a held mode carrying past_X or asking for next_X
        for frames, mode, extra, outputs in ((4, 1, [], ["normalized_motion"]), (0, 2, [], ["normalized_motion"]),
                                             (0, 1, [("past_k", np.zeros(STATE, np.float32))], ["normalized_motion"]),
                                             (4, 2, [("past_v", np.zeros(STATE, np.float32))], ["normalized_motion"]),
                                             (0, 1, [], ["next_k"]), (4, 2, [], ["normalized_motion", "next_v"])):
            c, _ = self.held_conn()
            if mode == 2:
                self.assertEqual(self.held_run(c, 8, 1, 1, 1, 0)[0], 4)  # a state there, so only the refusal is in the way
            kind, payload = self.held_run(c, 9, 1, mode, 1, frames, outputs=outputs, extra=extra)
            self.assertEqual((kind, error_code(payload)), (5, (9, 1)), (frames, mode, outputs))
            self.assertTrue(c.closed())
        c, _ = self.held_conn()
        c.send(run(9, "stateful.onnx", window(1), ["normalized_motion"], version=3, stream=1, mode=1))
        self.assertEqual(error_code(c.answer()[1]), (9, 1))
        # the panel's Keep line state off: every held mode is refused
        c, _ = self.held_conn(self.other_server("--no-held"))
        kind, payload = self.held_run(c, 9, 1, 1, 1, 0)
        self.assertEqual((kind, error_code(payload)), (5, (9, 1)))
        self.assertTrue(c.closed())

    def test_at_most_8_held_streams_the_least_recently_used_freed(self):
        c, _ = self.held_conn()
        for stream in range(1, 9):
            self.assertEqual(self.held_run(c, stream, stream, 1, stream, 0)[0], 4)
        self.assertEqual(self.held_run(c, 20, 1, 2, 1, 4)[0], 4)  # stream 1 used: stream 2 is now the least recent
        self.assertEqual(self.held_run(c, 21, 9, 1, 9, 0)[0], 4)  # a ninth start frees it
        kind, payload = self.held_run(c, 22, 2, 2, 2, 4)
        self.assertEqual((kind, error_code(payload)), (5, (22, 8)))
        for stream, frames in ((1, 8), (3, 4), (9, 4)):
            self.assertEqual(self.held_run(c, 30 + stream, stream, 2, stream, frames)[0], 4, stream)
        # a start on a stream replaces its state: the count begins again
        self.assertEqual(self.held_run(c, 40, 1, 1, 5, 0)[0], 4)
        kind, payload = self.held_run(c, 41, 1, 2, 5, 8)
        self.assertEqual((kind, error_code(payload)), (5, (41, 8)))

    def test_a_hello_that_drops_the_model_frees_its_state(self):
        c, _ = self.held_conn()
        self.assertEqual(self.held_run(c, 1, 3, 1, 1, 0)[0], 4)
        c.send(hello("sesame", [self.entry], version=3))
        self.assertEqual(c.answer()[0], 2)
        kind, payload = self.held_run(c, 2, 3, 2, 2, 4)
        self.assertEqual((kind, error_code(payload)), (5, (2, 3)))  # not ready on the connection
        c.send(hello("sesame", [self.entry, self.held_entry], version=3))
        self.assertEqual(c.answer()[0], 2)
        kind, payload = self.held_run(c, 3, 3, 2, 2, 4)
        self.assertEqual((kind, error_code(payload)), (5, (3, 8)))  # listed again, but its state was freed
        # a HELLO that still lists it keeps it
        self.assertEqual(self.held_run(c, 4, 3, 1, 1, 0)[0], 4)
        c.send(hello("sesame", [self.held_entry], version=3))
        self.assertEqual(c.answer()[0], 2)
        self.assertEqual(self.held_run(c, 5, 3, 2, 2, 4)[0], 4)

    def test_a_released_version_2_panel_refuses_a_version_3_hello(self):
        port = self.other_server("--v2-only")
        c = self.connect(port)
        c.send(hello("sesame", [self.entry, self.held_entry], version=3))
        kind, payload = c.answer()
        self.assertEqual((kind, error_code(payload), c.last_version), (5, (0, 6), 2))
        (n,) = struct.unpack_from("<H", payload, 6)
        self.assertEqual(payload[8:8 + n].decode(), "protocol version 3 - this server speaks versions 1 and 2")
        # read whole, then closed cleanly: an end of stream, not a reset
        c.s.settimeout(5)
        self.assertEqual(c.s.recv(1), b"")
        # the bridge then opens in version 2, which is answered, held aside
        c = self.connect(port)
        c.send(hello("sesame", [self.held_entry], version=2))
        kind, payload = c.answer()
        self.assertEqual((kind, c.last_version, [st for _, st, _, _ in parse_hello_ok(payload, 2)]), (2, 2, [0]))
        # and FIND, still a version 2 datagram, is answered
        u = Udp()
        self.addCleanup(u.close)
        self.assertIsNotNone(u.ask(self.other_server("--v2-only", "--remote-ip", "127.0.0.1"), find()))

    def test_notice_and_ping_on_a_version_3_connection(self):
        before = len(self.logged_notices())
        c, _ = self.held_conn()
        c.send(notice("stateful.onnx", "timeout", "no answer within 1000 ms", 1, version=3))
        c.send(frame(6, b"", version=3))
        self.assertEqual((c.answer(), c.last_version), ((7, b""), 3))
        self.assertEqual([n["model"] for n in self.logged_notices()[before:]], ["stateful.onnx"])

    def test_drop_held_on_demand_then_a_replay_rebuilds_the_line(self):
        trigger = os.path.join(tempfile.mkdtemp(), "drop")
        self.addCleanup(shutil.rmtree, os.path.dirname(trigger))
        port = self.other_server("--drop-held-on", trigger)
        seeds = [21, 22, 23, 24, 25]
        local = local_line(self.stateful, seeds)
        c, _ = self.held_conn(port)
        self.held_line(c, 4, seeds[:3])
        open(trigger, "w").close()
        frames = line_frames(4)[3]
        kind, payload = self.held_run(c, 50, 4, 2, seeds[3], frames)
        self.assertEqual((kind, error_code(payload)), (5, (50, 8)))
        self.assertFalse(os.path.exists(trigger))
        # the bridge's replay: window 0 as a start, the others as continues, then the window that got ERROR 8
        replayed = self.held_line(c, 4, seeds[:4], first_id=60)
        self.assertEqual(replayed[3].tobytes(), local[3].tobytes())
        kind, payload = self.held_run(c, 70, 4, 2, seeds[4], 8)
        self.assertEqual(parse_run_ok(payload)[2][0][1].tobytes(), local[4].tobytes())

    def test_drop_held_every_n_continues_spares_a_rebuilt_line(self):
        port = self.other_server("--drop-held-every", "3")
        seeds = [31, 32, 33, 34]
        c, _ = self.held_conn(port)
        self.held_line(c, 2, seeds[:3])  # continues 1 and 2
        kind, payload = self.held_run(c, 80, 2, 2, seeds[3], 8)  # the third: dropped
        self.assertEqual((kind, error_code(payload)), (5, (80, 8)))
        replayed = self.held_line(c, 2, seeds, first_id=90)  # three continues, none dropped: the line was rebuilt
        self.assertEqual(replayed[3].tobytes(), local_line(self.stateful, seeds)[3].tobytes())


if __name__ == "__main__":
    MODELS = sys.argv.pop(1)
    unittest.main(verbosity=2)
