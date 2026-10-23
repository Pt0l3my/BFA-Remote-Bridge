#!/usr/bin/env python3
"""A small reference A2F server: protocol versions 1 and 2 (docs/PROTOCOL.md), server side.

It is the bridge's test partner and a reference for a real server such as PandorumLLM's A2F server. It runs the
models with ONNX Runtime's Python package on the processor by default, one thread per connection
(socketserver.ThreadingTCPServer), one session per (model, overrides) built at a HELLO and warmed up with one run
of zeros, and one lock per session around its runs.

    python3 tools/reference_server.py --models <folder> [--host 127.0.0.1] [--port 7871] [--key KEY]
                                      [--v1-only] [--remote-ip ADDRESS] [--notice-log FILE]

It prints "LISTENING <port>" once it accepts connections (with --port 0 the system picks the port). The test
options --delay-ms, --fault and --run-error make it slow or wrong on purpose (--fault-count limits them to the first
runs). Needs onnxruntime and numpy.

As PandorumLLM's server does: each connection's requests are answered in order, late ones too; PING is answered with
PONG only after HELLO (before it, ERROR 1); after ERROR 1, 2, 5, 6 or 7 the connection is closed.

The versions: each connection is answered in the version its first frame opened it with, 1 or 2 (--v1-only: 1
alone, as PandorumLLM v3.97 Beta patch7 and earlier; --p6: the same, closing as p6 and earlier do, with a refused frame's
payload unread). A first frame of a version it doesn't speak gets ERROR 6, framed in the
newest version it speaks, and the connection closes; so does a later frame of another version. Version 2 adds
NOTICE (one-way, logged; --notice-log FILE appends each as a JSON line) and, with --remote-ip ADDRESS, FIND and FOUND
on UDP on the same port: a FIND from that address alone is answered with FOUND, at most one a second.

The sizes rule (version 2, and PandorumLLM's patch7 in version 1): a HELLO that lists no overrides for a file of
BFA 4.3.2.3's set gets that set's sizes, with the detail "built with BFA 4.3.2.3's sizes"; one that lists overrides
is served as listed. This server holds no such table: --fill-sizes gives one for its test files.
"""

import argparse
import hashlib
import hmac
import json
import os
import socket
import socketserver
import struct
import sys
import threading
import time

import numpy as np
import onnxruntime as ort

MAGIC = b"BFAR"
VERSIONS = (1, 2)  # the versions this server speaks; --v1-only: (1,)
HEADER = struct.Struct("<4sHHI")
MAX_PAYLOAD = 64 * 1024 * 1024

HELLO, HELLO_OK, RUN, RUN_OK, ERROR, PING, PONG = 1, 2, 3, 4, 5, 6, 7
FIND, FOUND, NOTICE = 8, 9, 10  # version 2: FIND and FOUND on UDP only, NOTICE one-way on TCP
FOUND_EVERY = 1.0  # at most one FOUND a second
# the row's detail when the sizes were filled in (the agreed words; with --fill-sizes the test files stand in for
# BFA 4.3.2.3's)
FILLED_DETAIL = "built with BFA 4.3.2.3's sizes"

READY, NOT_FOUND, DIFFERENT_FILE, FAILED_TO_LOAD, NOT_ALLOWED = 0, 1, 2, 3, 4

E_MALFORMED, E_WRONG_KEY, E_UNKNOWN_MODEL, E_INFERENCE, E_TOO_LARGE, E_VERSION, E_BUSY = 1, 2, 3, 4, 5, 6, 7
# after these the server closes the connection; 7 says it is busy or stopping
CLOSING_ERRORS = {E_MALFORMED, E_WRONG_KEY, E_TOO_LARGE, E_VERSION, E_BUSY}
MAX_NAME = 255  # bytes of a model or tensor name; longer is malformed

DTYPES = {1: np.dtype("<f4"), 6: np.dtype("<i4"), 7: np.dtype("<i8"), 10: np.dtype("<f2"), 11: np.dtype("<f8")}
CODES = {v: k for k, v in DTYPES.items()}
# BFA's own CPU sessions use "extended", never "all": the full level's layout fusions fail on the casts the CPU provider
# inserts around network_fp16.onnx's operators.
OPTIMIZATION = {
    "disabled": ort.GraphOptimizationLevel.ORT_DISABLE_ALL,
    "basic": ort.GraphOptimizationLevel.ORT_ENABLE_BASIC,
    "extended": ort.GraphOptimizationLevel.ORT_ENABLE_EXTENDED,
    "all": ort.GraphOptimizationLevel.ORT_ENABLE_ALL,
}

ORT_TYPES = {
    "tensor(float)": np.float32,
    "tensor(int32)": np.int32,
    "tensor(int64)": np.int64,
    "tensor(float16)": np.float16,
    "tensor(double)": np.float64,
}


class Refusal(Exception):
    """A request the server answers with ERROR."""

    def __init__(self, code, message="", request_id=0):
        super().__init__(message)
        self.code = code
        self.message = message
        self.request_id = request_id


def valid_name(name):
    if not name or name in (".", "..") or ".." in name or len(name.encode("utf-8")) > MAX_NAME:
        return False
    return not any(c in "/\\:" or ord(c) < 0x20 or ord(c) == 0x7F for c in name)


class Reader:
    def __init__(self, data):
        self.data = memoryview(data)
        self.at = 0

    def take(self, n):
        if n > len(self.data) - self.at:
            raise Refusal(E_MALFORMED, "truncated payload")
        b = self.data[self.at:self.at + n]
        self.at += n
        return b

    def u8(self):
        return self.take(1)[0]

    def u16(self):
        return struct.unpack("<H", self.take(2))[0]

    def u32(self):
        return struct.unpack("<I", self.take(4))[0]

    def u64(self):
        return struct.unpack("<Q", self.take(8))[0]

    def i64(self):
        return struct.unpack("<q", self.take(8))[0]

    def str(self):
        n = self.u16()
        try:
            return bytes(self.take(n)).decode("utf-8")
        except UnicodeDecodeError:
            raise Refusal(E_MALFORMED, "a string isn't UTF-8")

    def done(self):
        if self.at != len(self.data):
            raise Refusal(E_MALFORMED, "trailing bytes")


def pack_str(s):
    b = s.encode("utf-8")
    if len(b) > 0xFFFF:
        raise ValueError("string longer than 65,535 bytes")
    return struct.pack("<H", len(b)) + b


def frame(kind, payload, version=1):
    return HEADER.pack(MAGIC, version, kind, len(payload)) + payload


def error_payload(request_id, code, message):
    return struct.pack("<IH", request_id, code) + pack_str(message[:1000])


def read_tensor(r):
    name = r.str()
    if not name or len(name.encode("utf-8")) > MAX_NAME:
        raise Refusal(E_MALFORMED, "a tensor name is empty or longer than %d bytes" % MAX_NAME)
    code = r.u8()
    rank = r.u8()
    dims = [r.i64() for _ in range(rank)]
    length = r.u32()
    if code not in DTYPES:
        raise Refusal(E_MALFORMED, "unsupported tensor type %d" % code)
    if any(d < 0 for d in dims):
        raise Refusal(E_MALFORMED, "negative dimension")
    count = 1
    for d in dims:
        count *= d
    if count * DTYPES[code].itemsize != length:
        raise Refusal(E_MALFORMED, "byte length doesn't match the shape")
    data = r.take(length)
    return name, np.frombuffer(data, dtype=DTYPES[code]).reshape(dims)


def pack_tensor(name, array):
    array = np.ascontiguousarray(array)
    dtype = array.dtype.newbyteorder("<") if array.dtype.byteorder == ">" else array.dtype
    code = CODES.get(np.dtype(dtype).newbyteorder("<"))
    if code is None:
        raise Refusal(E_INFERENCE, "output %s has a type the protocol doesn't carry (%s)" % (name, array.dtype))
    data = array.astype(DTYPES[code], copy=False).tobytes()
    head = pack_str(name) + struct.pack("<BB", code, array.ndim) + b"".join(struct.pack("<q", d) for d in array.shape)
    return head + struct.pack("<I", len(data)) + data


def file_digest(path):
    h = hashlib.sha256()
    size = 0
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
            size += len(chunk)
    return size, h.digest()


class Models:
    """The model folder and the sessions built from it: one per (name, overrides), kept until the server stops."""

    def __init__(self, folder, providers, allowed=None, optimization="extended", threads=0, fill=None):
        self.folder = folder
        self.fill = fill or {}  # SHA-256 -> [(dim, value)]: the sizes filled in when a HELLO lists none
        self.threads = threads
        self.providers = providers
        self.optimization = optimization
        self.allowed = allowed
        self.lock = threading.Lock()
        self.sessions = {}
        self.digests = {}

    def digest(self, path):
        st = os.stat(path)
        key = (path, st.st_size, st.st_mtime_ns)
        with self.lock:
            if key in self.digests:
                return self.digests[key]
        value = file_digest(path)
        with self.lock:
            self.digests[key] = value
        return value

    def prepare(self, name, size, sha, overrides):
        """(status, detail, session key) for one HELLO model."""
        if self.allowed is not None and name not in self.allowed:
            return NOT_ALLOWED, "this server doesn't serve that model", None
        path = os.path.join(self.folder, name)
        if not os.path.isfile(path):
            return NOT_FOUND, "no such file in the model folder", None
        have_size, have_sha = self.digest(path)
        if have_size != size or have_sha != sha:
            return DIFFERENT_FILE, "the file here has another size or SHA-256", None
        # the sizes rule: a HELLO that lists none gets the set's own, for a file of that set alone (by SHA-256)
        filled = not overrides and sha in self.fill
        if filled:
            overrides = self.fill[sha]
        key = (name, tuple(overrides))
        with self.lock:
            entry = self.sessions.get(key)
            if entry is None:
                entry = {"lock": threading.Lock(), "session": None, "error": None}
                self.sessions[key] = entry
        with entry["lock"]:
            if entry["session"] is None and entry["error"] is None:
                try:
                    options = ort.SessionOptions()
                    options.graph_optimization_level = OPTIMIZATION[self.optimization]
                    if self.threads:
                        options.intra_op_num_threads = self.threads
                    for dim, value in overrides:
                        options.add_free_dimension_override_by_name(dim, value)
                    session = ort.InferenceSession(path, sess_options=options, providers=self.providers)
                    warm_up(session, dict(overrides))
                    entry["session"] = session
                except Exception as e:  # the runtime's own message, without this PC's folder
                    entry["error"] = self.scrub(e)
            if entry["error"] is not None:
                return FAILED_TO_LOAD, entry["error"], None
        return READY, FILLED_DETAIL if filled else "", key

    def scrub(self, error):
        """The runtime's message, its first line, with the model folder's path cut (a client never sees it)."""
        text = str(error).splitlines()[0] if str(error) else type(error).__name__
        for path in {os.path.abspath(self.folder), self.folder}:
            text = text.replace(path + os.sep, "").replace(path, "")
        return text[:200]

    def run(self, key, feeds, outputs):
        entry = self.sessions[key]
        with entry["lock"]:
            start = time.perf_counter()
            results = entry["session"].run(outputs, feeds)
            micros = int((time.perf_counter() - start) * 1e6)
        return results, micros


def warm_up(session, overrides):
    feeds = {}
    for i in session.get_inputs():
        shape = [d if isinstance(d, int) else overrides.get(d, 1) if isinstance(d, str) else 1 for d in i.shape]
        feeds[i.name] = np.zeros(shape, dtype=ORT_TYPES.get(i.type, np.float32))
    session.run(None, feeds)


class Handler(socketserver.StreamRequestHandler):
    def setup(self):
        super().setup()
        self.request.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.request.settimeout(self.server.idle_seconds)

    def read_exact(self, n):
        chunks = []
        while n > 0:
            b = self.rfile.read(n)
            if not b:
                return None
            chunks.append(b)
            n -= len(b)
        return b"".join(chunks)

    def send(self, kind, payload):
        self.wfile.write(frame(kind, payload, self.version))
        self.wfile.flush()

    def handle(self):
        ready = {}
        hello_done = False
        cfg = self.server.cfg
        spoken = self.server.versions
        self.version = None  # the connection's: its first frame's
        self.client = ""
        try:
            while True:
                head = self.read_exact(HEADER.size)
                if head is None:
                    return
                magic, version, kind, length = HEADER.unpack(head)
                if self.version is None:
                    # answered in the version it opened with; one this server doesn't speak, in the newest it does
                    self.version = version if magic == MAGIC and version in spoken else max(spoken)
                try:
                    if magic != MAGIC:
                        raise Refusal(E_MALFORMED, "bad magic")
                    if version != self.version:  # a version it doesn't speak, or not the connection's
                        # patch7 reads the whole frame first and closes cleanly; p6 (--p6) closes with the payload
                        # unread, which can reset the connection before its ERROR 6 is read
                        if not cfg.p6 and length <= MAX_PAYLOAD and length:
                            self.read_exact(length)
                        raise Refusal(E_VERSION, "protocol version %d not supported" % version)
                    if length > MAX_PAYLOAD:
                        # answered and closed: the oversize payload is never read
                        self.send(ERROR, error_payload(0, E_TOO_LARGE, "payload above 64 MiB"))
                        return
                    payload = self.read_exact(length) if length else b""
                    if payload is None:
                        return
                    if kind == HELLO:
                        ready = self.hello(payload)
                        hello_done = True
                    elif not hello_done:
                        raise Refusal(E_MALFORMED, "HELLO comes first")
                    elif kind == PING:
                        if length:
                            raise Refusal(E_MALFORMED, "PING carries nothing")
                        self.send(PONG, b"")
                    elif kind == RUN:
                        self.run(payload, ready, cfg)
                    elif kind == NOTICE and self.version >= 2:
                        self.notice(payload)  # one-way: nothing is sent back
                    else:
                        raise Refusal(E_MALFORMED, "unexpected message type %d" % kind)
                except Refusal as e:
                    self.send(ERROR, error_payload(e.request_id, e.code, e.message))
                    if e.code in CLOSING_ERRORS:
                        return
        except (ConnectionError, OSError):
            return

    def hello(self, payload):
        r = Reader(payload)
        client, key, ort_version = r.str(), r.str(), r.str()
        models = []
        for _ in range(r.u16()):
            name = r.str()
            size = r.u64()
            sha = bytes(r.take(32))
            overrides = [(r.str(), r.i64()) for _ in range(r.u16())]
            if not valid_name(name):
                raise Refusal(E_MALFORMED, "a model name isn't a plain file name")
            models.append((name, size, sha, overrides))
        r.done()
        self.client = client
        expected = self.server.key
        if expected and not hmac.compare_digest(key.encode("utf-8"), expected.encode("utf-8")):
            raise Refusal(E_WRONG_KEY, "wrong access key")
        ready = {}
        out = pack_str(self.server.name) + pack_str(self.server.device) + struct.pack("<H", len(models))
        for name, size, sha, overrides in models:
            status, detail, key = self.server.models.prepare(name, size, sha, overrides)
            if status == READY:
                ready[name] = key
            out += pack_str(name) + struct.pack("<B", status) + pack_str(detail)
        self.server.log("HELLO from %s (runtime %s, version %d): %d models" % (client, ort_version, self.version, len(models)))
        self.send(HELLO_OK, out)
        return ready

    def notice(self, payload):
        """NOTICE: logged, never answered; one it can't read is ignored."""
        try:
            r = Reader(payload)
            model, reason, detail, lines = r.str(), r.str(), r.str(), r.u32()
            r.done()
            if not valid_name(model):
                raise Refusal(E_MALFORMED, "the model name isn't a plain file name")
        except Refusal as e:
            self.server.log("NOTICE ignored: %s" % e.message)
            return
        self.server.log("NOTICE from %s: %s, %s (%d lines): %s" % (self.client, model, reason, lines, detail))
        self.server.record_notice({"client": self.client, "model": model, "reason": reason, "detail": detail, "lines": lines})

    def run(self, payload, ready, cfg):
        r = Reader(payload)
        request_id = r.u32()
        faulty = self.server.take_fault()
        try:
            model = r.str()
            if not valid_name(model):
                raise Refusal(E_MALFORMED, "the model name isn't a plain file name")
            feeds = {}
            for _ in range(r.u16()):
                name, array = read_tensor(r)
                feeds[name] = array
            outputs = [r.str() for _ in range(r.u16())]
            r.done()
            if any(not n or len(n.encode("utf-8")) > MAX_NAME for n in outputs):
                raise Refusal(E_MALFORMED, "an output name is empty or longer than %d bytes" % MAX_NAME)
        except Refusal as e:
            e.request_id = request_id
            raise
        if model not in ready:
            raise Refusal(E_UNKNOWN_MODEL, "%s isn't ready here" % model, request_id)
        if faulty and cfg.delay_ms:
            time.sleep(cfg.delay_ms / 1000.0)
        if faulty and cfg.run_error:
            raise Refusal(cfg.run_error, "a test option's error", request_id)
        try:
            results, micros = self.server.models.run(ready[model], feeds, outputs)
        except Exception as e:
            raise Refusal(E_INFERENCE, self.server.models.scrub(e), request_id)
        fault = cfg.fault if faulty else "none"
        if fault == "malformed":
            self.send(RUN_OK, struct.pack("<II", request_id, micros) + b"\x01")
            return
        rid = request_id + 1 if fault == "request-id" else request_id
        names = list(outputs)
        if fault == "output-name" and names:
            names[0] = names[0] + "_other"
        if fault == "output-type" and results:
            results = list(results)
            results[0] = np.asarray(results[0]).astype(np.float64 if np.asarray(results[0]).dtype != np.float64 else np.float32)
        body = struct.pack("<IIH", rid, micros, len(results))
        for name, value in zip(names, results):
            body += pack_tensor(name, np.asarray(value))
        if len(body) > MAX_PAYLOAD:
            raise Refusal(E_TOO_LARGE, "the answer is above 64 MiB", request_id)
        self.send(RUN_OK, body)


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def record_notice(self, notice):
        if not self.cfg.notice_log:
            return
        with self.notice_lock, open(self.cfg.notice_log, "a", encoding="utf-8") as f:
            f.write(json.dumps(notice) + "\n")

    def take_fault(self):
        """Whether this RUN gets the test options' delay, fault or error (all of them, or the first --fault-count)."""
        with self.fault_lock:
            if self.cfg.fault_count <= 0:
                return True
            if self.faults_used < self.cfg.fault_count:
                self.faults_used += 1
                return True
            return False


class Finder(threading.Thread):
    """FIND and FOUND on UDP, on the TCP port: a FIND from the Remote IP alone is answered, at most once a second."""

    def __init__(self, server, host, port, remote_ip):
        super().__init__(daemon=True)
        self.server = server
        self.remote_ip = remote_ip
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind((host, port))
        self.last = None

    def run(self):
        while True:
            try:
                data, sender = self.sock.recvfrom(2048)
            except OSError:
                return
            self.answer(data, sender)

    def answer(self, data, sender):
        # anything but a well-formed version 2 FIND is ignored, never answered
        if len(data) < HEADER.size:
            return
        magic, version, kind, length = HEADER.unpack_from(data)
        if magic != MAGIC or version != 2 or kind != FIND or length != len(data) - HEADER.size:
            return
        try:
            r = Reader(data[HEADER.size:])
            client = r.str()
            r.done()
        except Refusal:
            return
        if sender[0] != self.remote_ip:
            self.server.log("FIND ignored: not from the Remote IP")
            return
        now = time.monotonic()
        if self.last is not None and now - self.last < FOUND_EVERY:
            self.server.log("FIND ignored: one FOUND a second at most")
            return
        self.last = now
        self.sock.sendto(frame(FOUND, pack_str(self.server.name), 2), sender)
        self.server.log("FIND from %s: answered with FOUND" % client)


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--models", required=True, help="the folder holding the .onnx files")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=7871)
    p.add_argument("--key", default="", help="the access key; empty accepts any client")
    p.add_argument("--providers", default="CPUExecutionProvider", help="comma-separated ONNX Runtime providers")
    p.add_argument("--optimization", default="extended", choices=sorted(OPTIMIZATION), help="the sessions' graph optimisation level")
    p.add_argument("--threads", type=int, default=0,
                   help="intra-op threads per session (0: the runtime's choice); BFA's own CPU sessions use 2, and the same "
                        "count gives results identical to BFA's bit for bit, where another count may differ in the last bit of "
                        "a float16 model's outputs")
    p.add_argument("--allow", default=None, help="comma-separated model names this server serves (default: any)")
    p.add_argument("--idle-seconds", type=float, default=0.0, help="close a connection idle this long (0: never, as PandorumLLM's)")
    p.add_argument("--quiet", action="store_true")
    p.add_argument("--delay-ms", type=int, default=0, help="test option: wait this long before answering each RUN")
    p.add_argument("--fault", default="none", choices=["none", "malformed", "request-id", "output-name", "output-type"],
                   help="test option: answer RUN wrongly")
    p.add_argument("--run-error", type=int, default=0, help="test option: answer RUN with this ERROR code")
    p.add_argument("--fault-count", type=int, default=0,
                   help="test option: --delay-ms, --fault and --run-error apply to the first this many RUNs only (0: all)")
    p.add_argument("--v1-only", action="store_true",
                   help="speak protocol version 1 alone, as PandorumLLM v3.97 Beta patch7 and earlier: a version 2 HELLO gets ERROR 6, "
                        "read whole first and closed cleanly, as patch7 does")
    p.add_argument("--p6", action="store_true",
                   help="speak protocol version 1 alone, as PandorumLLM v3.97-p6 and earlier: a version 2 HELLO's header alone is "
                        "read, ERROR 6 sent, and the connection closed with the payload unread (which can reset it)")
    p.add_argument("--remote-ip", default="",
                   help="answer FIND (UDP, on the same port) from this address alone, as PandorumLLM's Remote IP (empty: no FIND)")
    p.add_argument("--notice-log", default="", help="append each NOTICE to this file, one JSON object a line")
    p.add_argument("--fill-sizes", action="append", default=[], metavar="SHA256:DIM=VALUE[,DIM=VALUE]",
                   help="the sizes rule, for test files: a HELLO that lists no overrides for the file of this SHA-256 (hex) "
                        "gets these, and the row's detail says so; repeatable")
    cfg = p.parse_args(argv)

    providers = [x for x in cfg.providers.split(",") if x]
    allowed = set(cfg.allow.split(",")) if cfg.allow else None
    server = Server((cfg.host, cfg.port), Handler)
    server.cfg = cfg
    server.versions = (1,) if cfg.v1_only or cfg.p6 else VERSIONS
    if cfg.p6:
        Handler.rbufsize = 0  # reads take only what they ask for: the payload stays unread in the socket
    server.notice_lock = threading.Lock()
    server.key = cfg.key
    server.idle_seconds = cfg.idle_seconds or None
    server.fault_lock = threading.Lock()
    server.faults_used = 0
    fill = {}
    for spec in cfg.fill_sizes:
        digest, _, dims = spec.partition(":")
        fill[bytes.fromhex(digest)] = [(d, int(v)) for d, _, v in (x.partition("=") for x in dims.split(",") if x)]
    server.models = Models(cfg.models, providers, allowed, cfg.optimization, cfg.threads, fill)
    server.name = "BFA Remote Bridge reference server (onnxruntime %s)" % ort.__version__
    server.device = ", ".join(providers)
    server.log = (lambda line: None) if cfg.quiet else (lambda line: print(line, file=sys.stderr, flush=True))
    # FIND and FOUND are version 2's, and only with a Remote IP set, as PandorumLLM's
    finder = Finder(server, cfg.host, server.server_address[1], cfg.remote_ip) if cfg.remote_ip and not (cfg.v1_only or cfg.p6) else None
    if finder:
        finder.start()
    print("LISTENING %d" % server.server_address[1], flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
        if finder:
            finder.sock.close()


if __name__ == "__main__":
    main()
