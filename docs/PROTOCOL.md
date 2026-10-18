# The bridge's protocol, version 1

BFA Remote Bridge (the client, on the gaming PC) and an A2F server (on another PC of the local network, such as
PandorumLLM's) speak this protocol. `src/core/Protocol.h` implements both sides' encoding; `src/core/Client.h` the
client's conduct; `tools/reference_server.py` is a reference server.

## 1 Facts from Better Facial Animations 4.3.2

From BFA 4.3.2's public source (GPL-3.0; read, never copied):

- BFA loads `onnxruntime.dll` (ONNX Runtime **1.24.4**, DirectML build, `ORT_API_VERSION` **24**) from its own folder,
  `SKSE/Plugins/BetterFacialAnimations/`, with `DirectML.dll` and `onnxruntime_providers_shared.dll` beside it. It
  does so only after a save loads, on its own worker thread.
- It runs the model on **its own worker thread**, with **ordinary CPU memory** in and out, even in GPU mode
  (`CreateTensorWithDataAsOrtValue` over host buffers, `Run`, then reading the outputs). Nothing it hands to the
  runtime lives on the GPU, so a call can be forwarded byte for byte.
- It creates each session with `CreateSession(env, path, options)`. In GPU mode it fixes some dynamic dimensions with
  `AddFreeDimensionOverrideByName` (listed below); its CPU sessions set none. The server applies exactly the overrides
  the client lists.
- Right after creating a session it runs it once on all-zero inputs (its warm-up), and drops the model when that run
  fails.
- It checks every output before use: the element count, the type (float) and that every value is finite. For the
  facial models a failed check fails that line only ("the session and device stay"); an error status from `Run` marks
  the session failed. The sentiment model (`emotion_fp16.onnx`) is the exception: BFA drops its session after any
  failed answer, checked or not, and loads it again 30 seconds later on the processor, 60 on the graphics card.
- Each model is **stateless between calls**: V3's recurrent state travels in its inputs and outputs (`input_latents`
  in, `output_latents` out), so the server keeps nothing per conversation.

The models (file names as BFA ships them; sizes per call):

| File | BFA setting | Inputs (name: shape, type) | Outputs | Free-dimension overrides (GPU mode) |
|---|---|---|---|---|
| `a2f3_fp16.onnx` (Audio2Face-3D v3.0, folded) | V3 (default) | `window`: [1, 16000] f32; `identity`: [1, 3] f32; `emotion`: [1, 30, 10] f32; `input_latents`: [2, 2, 1, 256] f32 | `skin_latent` (30×512 f32), `tongue_latent` (30×512 f32), `jaw_eyes` (30×19 f32), `output_latents` (1024 f32) | `window_dynamic_axes_1`, `identity_dynamic_axes_1`, `emotion_dynamic_axes_1`, `input_latents_dynamic_axes_1` = 1 |
| `performance.onnx` (Performance "Thin120") | Performance | `mel`: [1, F, 80] f32; `identity`: [1] **int64**; `emotion`: [1, F, 10] f32 | `normalized_motion` (F×73 f32) | `batch` = 1 |
| `network_fp16.onnx` (Audio2Face-3D v2.3 Mark) | Old | `input`: [b, 1, 8320] f32; `emotion`: [b, 1, 26] f32 | `result` ([b, 1, 301] f32) | `input_dynamic_axes_1`, `emotion_dynamic_axes_1` = BFA's batch (2 by default) |
| `emotion_fp16.onnx` (Audio2Emotion v2.2) | Sentiment | `input_values`: [1, 60000] f32 | `output` (6 f32) | `batch_size` = 1, `seq_len` = 60000 |

V3 runs **one 30-frame window (half a second of speech) per call**: about 70 KB up and 130 KB back. A 6-second
SkyrimNet chunk is about 12 calls in sequence (each needs the previous call's `output_latents`), so about 2.4 MB in
all: trivial on a LAN.

## 2 The protocol

TCP, one request in flight per connection, strictly request then response. All integers little-endian.

**Strings** (`str`): `u16` byte length, then UTF-8 bytes, no terminator.

**Frame**: a 12-byte header, then the payload.

| Bytes | Field |
|---|---|
| 4 | magic, the ASCII bytes `BFAR` |
| 2 | `u16` protocol version, `1` |
| 2 | `u16` message type |
| 4 | `u32` payload length (at most 64 MiB; larger is refused) |

**Message types**

| Type | Name | Direction | Payload |
|---|---|---|---|
| 1 | HELLO | client → server | see below |
| 2 | HELLO_OK | server → client | see below |
| 3 | RUN | client → server | see below |
| 4 | RUN_OK | server → client | see below |
| 5 | ERROR | server → client | `u32` request id (0 for HELLO), `u16` code, `str` message |
| 6 | PING | client → server | empty |
| 7 | PONG | server → client | empty |

**HELLO** comes first on every connection, and may come again at any time (BFA can switch models mid-game: the
bridge then sends its current list again).

```
str  client          e.g. "BFARemoteBridge 1.0.0"
str  key             the access key, empty if none
str  ort_version     the gaming PC's runtime, e.g. "1.24.4" (informational)
u16  model_count
  per model:
  str  name          the file's base name, e.g. "a2f3_fp16.onnx" (no path, no separators)
  u64  size          the file's size in bytes
  u8[32] sha256      the file's SHA-256
  u16  override_count
    per override: str dim_name, i64 value
```

**HELLO_OK**

```
str  server          e.g. "PandorumLLM v3.98 A2F"
str  device          e.g. "DirectML 0: NVIDIA GeForce RTX 3090"
u16  model_count     the same models, in the same order
  per model:
  str  name
  u8   status        0 ready, 1 not found, 2 different file (size or SHA-256), 3 failed to load, 4 not allowed
  str  detail        a short reason when status isn't 0, else empty
```

The server answers **ready** only when it holds a file with the **same name, size and SHA-256**, and has a session
built with **exactly the overrides listed**, warmed up (one run of zeros, as BFA does). A model that isn't ready is
never run.

**RUN**

```
u32  request_id      the bridge's counter
str  model           a name from the last HELLO
u16  input_count
  per input:
  str  name
  u8   dtype         ONNX TensorProto codes: 1 float, 7 int64 (BFA uses only these two); also 6 int32, 10 float16, 11 double
  u8   rank
  i64  dims[rank]
  u32  byte_length   must equal the element count × the type's size
  u8   data[byte_length]
u16  output_count
  per output: str name
```

**RUN_OK**

```
u32  request_id      echoed
u32  server_micros   time spent in the runtime's Run, for the panel's statistics and the bridge's log
u16  output_count    in the order requested
  per output:
  str  name
  u8   dtype
  u8   rank
  i64  dims[rank]
  u32  byte_length
  u8   data[byte_length]
```

**Error codes**: 1 malformed frame or payload; 2 wrong access key; 3 unknown or unready model; 4 inference failed
(the runtime's message in `message`); 5 too large; 6 protocol version not supported; 7 busy or shutting down. After
codes 1, 2 and 6 the server closes the connection, and after code 5 for a frame header announcing more than 64 MiB
(whose payload it never reads); after the others it stays open.

**Rules**

- Model names are matched against files in the configured folder by exact base name. A name with a path separator,
  `..` or a drive letter is refused (code 1). The client never sends a path, and the server's messages name no path
  of its own.
- The access key, when set, is compared in constant time; an empty server key accepts any client.
- The server answers PING with PONG at any time after HELLO. It may close a connection idle for more than 10
  minutes; the bridge reconnects as needed.
- The server answers BFA's warm-up run (all-zero inputs, right after the session is made) like any other RUN.
- A ready model's `detail` may say something too, for instance that another model runs in its place ("runs
  Audio2Face-3D v2.3 Mark (NVIDIA FP32 v2.3) in its place"); the bridge logs every non-empty detail.
- The client never sends PING before HELLO (the bridge sends none at all); a server may refuse one with code 1.

**The server's duties at loading**

- A HELLO that lists a model the server hasn't loaded yet makes it load and warm the model up before answering (one
  to a few seconds for a DirectML session). The bridge waits up to 60 seconds for that answer, at a model load only.
- The server **finishes a model load even if the client disconnects** meanwhile, and **keeps loaded sessions across
  reconnections**: a later HELLO for the same model and overrides, on any connection, is answered at once. The bridge
  relies on it: the HELLO it says before a run (after a reconnection, or when the live models changed) waits only for
  that run's timeout.

## 3 What the bridge does

The bridge takes BFA's model inference over completely while it is on: no model runs on the gaming PC, on its
graphics card or its processor. A late lip sync looks worse than none, and inference on the processor stutters too,
so there is no local fallback: a line the server doesn't answer in time gets the game's own lip sync.

- **Settings** on SkyrimNet's Settings > Plugins page: **Bridge** (on or off), **Server address**, **Port** (default
  7871), **Access key**, **Timeout** (ms per call, default 1000, 200 to 5000), **Check results**, **Status on load**,
  **Log detail**. The bridge is on when Bridge is ticked and a server address is set.
- **Attaching.** At SKSE's post-load message it finds `BetterFacialAnimations.dll`, reads its version from its
  `SKSEPlugin_Version` data (4.3.x only: any other BFA runs as if the bridge weren't installed), and patches the entry for KERNEL32's `GetProcAddress` in BFA's import address table. Its
  replacement changes one answer only: BFA's lookup of `OrtGetApiBase` in `onnxruntime.dll` gets the bridge's. That
  one's `GetApi(24)` is a copy of the real `OrtApi` table with a few entries replaced; any other API version gets the
  real table, untouched, and BFA runs as if the bridge weren't installed.
- **A model load** (`CreateSession`, while the bridge is on) never reaches the real runtime. The bridge hashes the
  model file (base name, size, SHA-256), takes the overrides BFA set on the options, connects and sends HELLO listing
  every live model and the new one.
  - Ready on the server: BFA gets a session handle of the bridge's own, with success. Nothing is loaded on the gaming
    PC. A newer session of a model replaces older ones in HELLO (BFA can drop a session without releasing it); a run
    on a replaced session gets the NaN answer below, without a backoff.
  - Not (no answer, the wrong key, not found, a different file, failed to load, not allowed): BFA gets an error status
    made by the real `CreateStatus(ORT_FAIL, "BFA Remote Bridge: ...")` naming the reason. BFA then has no model, and
    lip sync is the game's own until BFA loads its model again (a restart, or a model switch in BFA's menu).
- **A run** on a bridge session, BFA's warm-up run included, always goes to the server: RUN, then RUN_OK checked
  against the call (the count, the names and order, and the types and shapes of any outputs the caller passed), then
  the outputs handed to BFA as the runtime would (new values made by the real `CreateTensorAsOrtValue`, or the
  caller's filled).
  - When the server doesn't answer in full (a timeout, a lost connection, an ERROR, a malformed or mismatched
    answer), the call still succeeds, with each requested output a one-element float tensor holding NaN. BFA's output
    check rejects it and that one line plays with the game's own lip sync; an error status would make BFA drop its
    model. A 10-second backoff follows, during which runs answer so at once, without touching the network; the first
    run after it reconnects and says HELLO again. One log line per backoff.
  - HELLO is sent again before a run whenever the set of live models changed since the last one, or after a failure.
- **Before the settings are read**: SkyrimNet registers the bridge's settings during its own start-up, so the bridge
  reads them at the data-loaded message, when the main menu opens, before each game loads and when its page is
  saved, and counts them read once SkyrimNet names the config. A model load before that is refused ("the bridge's
  settings weren't read yet"; BFA tries a failed load again 30 seconds later) while SkyrimNet is loaded, and loads on
  the gaming PC as usual when SkyrimNet isn't. Every model load that passes through is logged with its reason.
- **The server's name** is looked up once per change of the address or port, off the game's threads, and connections
  use the addresses found. A failed lookup is logged once, and runs get the NaN answer until the settings change.
- **A reused connection the server has closed** (it may close one idle for 10 minutes) is opened again within the same
  call: when sending fails or the connection closes before any byte of the answer, the bridge reconnects, says HELLO
  and sends again, once, within that call's deadline.
- **Every other entry that takes a session** (the 22 of `OrtApi` besides `Run`, such as `SessionGetInputCount`,
  `SessionGetOutputName`, `CreateIoBinding`, `RunWithBinding` and `RunAsync`, and the model editor's three) answers a
  bridge session with an error status made by `CreateStatus`, without the runtime. BFA calls none of them.
- **Releasing** a bridge session frees it in the bridge; it never reaches the runtime. Sessions made while the bridge
  was off are the runtime's own, and every call on them is the real one: the on/off setting takes effect at BFA's
  next model load.
- **Everything else** BFA calls (options, providers, tensors, memory information, statuses) is the real runtime's. In
  GPU mode BFA still opens its own Direct3D 12 device before creating a session (its own code), but no inference runs
  on it; the bridge logs that BFA is set to GPU, and Force CPU avoids even that device.
- **Check mode** (Check results above 0) is a diagnostic that does run the model on the gaming PC: only while it is
  on, a model load also makes the real session, and each session's first N runs run both ways, the largest difference
  per output logged.
- **The log**, `BFARemoteBridge.log` in SKSE's log folder: BFA found or not, its version, the attachment, the settings
  (the address and the key only as set or not), each model's size, SHA-256 and overrides, the server's answer to each
  HELLO, each failure and its backoff, and every 250 answered runs the round trip and server times. Log detail
  "Detailed" adds a line per run.
