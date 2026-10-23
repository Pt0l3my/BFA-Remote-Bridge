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
str  client          e.g. "BFARemoteBridge 1.2.0 (answered 1250, fallbacks 0, timeouts 0, BFA 4.3.2.3, ORT 1.24.4)"
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

The bridge's **client string** is its name and version, then its keys in parentheses: `BFARemoteBridge 1.2.0
(answered N, fallbacks N, timeouts N, fallback REASON, BFA V, ORT V)`.

- The counters are cumulative from Skyrim's start: `answered` counts the runs the server answered, `fallbacks` every
  run it didn't (the line got the game's own lip sync), and `timeouts` the part of those that timed out.
- `fallback REASON` (since 1.2.0) comes only in a status check the bridge sends because a line fell back: REASON is
  one word, `unreachable`, `paused`, `timeout`, `error`, `notready`, `refused` or `incompatible`
  (`src/core/Reasons.h`; a word, once released, never changes).
- `BFA V` and `ORT V` (since 1.2.0) are the gaming PC's BFA version (from its DLL) and the runtime BFA loaded, each
  only once known.

The keys keep this order; new keys are only ever added at the end. A server may show the string as given or parse it
by its key words. The string stays within **120 characters**, which PandorumLLM's log lines keep since its patch4 (80
before). The three counts fit whole in any case (116 characters at most); the example is 87. When the keys would make
it longer, the trailing keys are dropped, last first, until it fits (so `ORT`, then `BFA`, then `fallback`), and the
name and version alone are sent when no key fits.

**HELLO_OK**

```
str  server          e.g. "PandorumLLM v3.97-p4 Beta A2F"
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
codes 1, 2, 5 and 6 the server closes the connection; code 7 says it is stopping, and the bridge treats it as the
connection's end too. After codes 3 and 4 the connection stays open.

**Rules**

- Model names are matched against files in the configured folder by exact base name. A name with a path separator,
  `..` or a drive letter is refused (code 1). The client never sends a path, and the server's messages name no path
  of its own.
- The access key, when set, is compared in constant time; an empty server key accepts any client.
- The server answers each connection's requests **in order**, one at a time, late answers too: after a request the
  client stopped waiting for, that request's RUN_OK or ERROR still comes, before the next answer.
- The server answers PING with PONG at any time after HELLO. A PING before HELLO is ERROR 1, and the connection
  closes. A server may close an idle connection (PandorumLLM no longer does since its patch3, and uses TCP
  keepalive); the bridge PINGs a connection idle for 60 seconds and reconnects as needed.
- The server answers BFA's warm-up run (all-zero inputs, right after the session is made) like any other RUN.
- A ready model's `detail` may say something too, for instance that another model runs in its place ("runs
  Audio2Face-3D v2.3 Mark (NVIDIA FP32 v2.3) in its place"); the bridge logs every non-empty detail.
- The client never sends PING before HELLO: the bridge sends one only on a connection whose HELLO was answered and
  listed models, never on a status check's connection or a model load's.

**The server's duties at loading**

- A HELLO that lists a model the server hasn't loaded yet makes it load and warm the model up before answering (one
  to a few seconds for a DirectML session; 2.5 s for the sentiment model the first time, in the owner's logs). The
  bridge sends that HELLO on a **side connection** of its own and waits up to 60 seconds for the answer there, so the
  main connection is never held by a load.
- The server **finishes a model load even if the client disconnects** meanwhile, and **keeps loaded sessions across
  reconnections**: a later HELLO for the same model and overrides, on any connection, is answered at once. The bridge
  relies on it: after the side connection's HELLO_OK, the main connection's HELLO with every live model is answered
  at once. A second HELLO on a connection only replaces that connection's ready set; the server never unloads.
- One model loads at a time, across all connections, and a RUN on another connection runs meanwhile (the server
  takes only that session's lock). Since PandorumLLM's patch4 a **HELLO** of models already loaded is answered at
  once, even while another connection loads; a HELLO that has to load waits for a load in progress. In patch3 every
  HELLO waited, even one naming only loaded models, so the bridge's background thread holds its own HELLOs while the
  bridge's side load is in flight: with patch4 that costs nothing, and it keeps the bridge right with patch3. A status
  check names no model and never waits.
- **Not measured yet: DirectML during a first load.** On the server's graphics card a load's work (the weights read
  and uploaded, the warm-up) shares the card with the runs of the models already loaded. Nothing on the server makes
  those runs wait, but the card may slow them: lines during the first load of a model may fall back.

## 3 What the bridge does

The bridge takes BFA's model inference over completely: no model runs on the gaming PC, on its graphics card or its
processor. A late lip sync looks worse than none, and inference on the processor stutters too, so there is no local
fallback: a line the server doesn't answer in time gets the game's own lip sync. The hardware the server runs on is
PandorumLLM's choice; the bridge reports it.

- **Settings** on SkyrimNet's Settings > Plugins page, only what the server can't tell the bridge: **Server address**
  and **Port** (default 7871), for a server that doesn't answer discovery yet (today's), **Status on load** and **Log
  detail**. 1.1's Bridge, Access key, Timeout and Check results are gone; their saved values are ignored. The access
  key is empty, and each call's timeout is 1000 ms.
- **Attaching.** At SKSE's post-load message it finds `BetterFacialAnimations.dll`, reads its version from its
  `SKSEPlugin_Version` data (for the log and the client string only: any version is tried), and patches the entry for
  KERNEL32's `GetProcAddress` in BFA's import address table. Its replacement changes one answer only: BFA's lookup of
  `OrtGetApiBase` in `onnxruntime.dll` gets the bridge's. When the patch can't be made (no such import, BFA's runtime
  loaded already), BFA runs as it would without the bridge, and a note says so.
- **The API version BFA asks for** (`GetApi(N)`) is served from the real table:
  - N up to 24: a copy of the runtime's entries, only as many as the runtime's own API version (read from its
    `GetVersionString`) has: OrtApi is append-only, and the bridge knows each version's first entry from 1.11 on (by
    its `\since` note in the 1.24.4 header; each version's count is also checked against ONNX Runtime's own
    published sizes, such as 390 entries for 1.23), so it never reads past a shorter table. The entries below are replaced,
    each only where the runtime has it; the device check on tensors (`MemoryInfoGetDeviceType`, since 1.14) is
    skipped for an older runtime.
  - N from 25 to 30, of a runtime that has it: 1.24.4's entries as above, then every entry past them answered by the
    bridge with an error status naming it (`ORT_NOT_IMPLEMENTED`, "ONNX Runtime entry K is newer than the runtime the
    bridge knows"), never the runtime's. The bridge can't tell which of them could meet its sessions or values, so
    none of them reaches the runtime: whatever needs one fails safely, and the log and the monitor name it once. The
    stubs take no parameters: both x64 calling conventions leave the arguments to the caller. One limit: an entry
    that returns something other than a status (a table, a string) gets a status pointer, which BFA would misread;
    the log names the entry first.
    `GetExperimentalFunction` (index 422, since 1.28) returns a function pointer: its stub answers null, its
    documented "not available".
  - A runtime older than API version 10 (whose table length the header can't tell), one whose version can't be read,
    or N past 30: known to be incompatible, so `GetApi` answers null. BFA then has no model (its own "wrong
    version"), lip sync is the game's own, nothing runs on this PC, and a note says to update the bridge (or BFA, for
    a runtime older than 1.10).
  - The runtime's other session makers (`CreateSessionFromArray`, `CreateSessionWithPrepackedWeightsContainer`,
    `CreateSessionFromArrayWithPrepackedWeightsContainer`, and the model editor's `CreateSessionFromModel`,
    `CreateModelEditorSession` and `CreateModelEditorSessionFromArray`) answer with an error status naming them,
    without the runtime, logged once with a note: a model loaded so would run on this PC.
  - The model editor's table is served only to BFA asking at most 24 of a runtime at least that new (its layout is
    1.24.4's); otherwise `GetModelEditorApi` answers null, as a minimal build does. The DirectML table is wrapped
    only at version 24 of such a runtime.
- **A model load** (`CreateSession`) never reaches the real runtime. The bridge hashes the
  model file (base name, size, SHA-256), takes the overrides BFA set on the options, and sends HELLO with that model
  alone on a **side connection** of its own (3000 ms to connect, 60 seconds for the answer), closed after the answer.
  - Ready on the server: BFA gets a session handle of the bridge's own, with success. Nothing is loaded on the gaming
    PC. The main connection then gets a HELLO listing every live model (from the background thread, or the next run
    if it comes first), answered at once. A newer session of a model replaces older ones in HELLO (BFA can drop a
    session without releasing it); a run on a replaced session gets the NaN answer below.
  - Not (no answer, the wrong key, not found, a different file, failed to load, not allowed), or no server address
    set: BFA gets an error status made by the real `CreateStatus(ORT_FAIL, "BFA Remote Bridge: ...")` naming the
    reason. BFA then has no model, and lip sync is the game's own until BFA loads its model again (a restart, a model
    switch in BFA's menu, or BFA's own retry). A refusal met again on BFA's retries is logged once until it changes.
- **A run** on a bridge session, BFA's warm-up run included, always goes to the server, on the **main connection**:
  RUN, then RUN_OK checked against the call (the count, the names and order, and the types and shapes of any outputs
  the caller passed), then the outputs handed to BFA as the runtime would (new values made by the real
  `CreateTensorAsOrtValue`, or the caller's filled). HELLO goes first only when the connection has no answer for that
  model yet (a new connection).
  - **Its timeout** counts from the moment BFA makes the call, and bounds all of it: the wait for the main connection
    (another run, or the background thread's PING or HELLO, may hold it), a connect (only as long as the call has
    left, never the 3000 ms off the speech path), HELLO and RUN share that one deadline. A call that gets the
    connection with no time left sends nothing.
  - When the server doesn't answer in full (a timeout, a lost connection, an ERROR, a malformed or mismatched answer,
    a model not ready), the call still succeeds, with each requested output a one-element float tensor holding NaN.
    BFA's output check rejects it and that one line plays with the game's own lip sync; an error status would make
    BFA drop its model. Nothing runs on the gaming PC for it.
- **A connection the server closed or reset, or a send that failed** (a reused connection) is opened again within the
  same call: the bridge reconnects, says HELLO and sends the same RUN, once, within that call's deadline. The log says
  which of the three it was.
- **A RUN timeout** with no byte of its answer arrived keeps the connection. The server answers in order, so that
  request's late RUN_OK or ERROR comes before the next answer: the next exchange on the connection (a RUN, a HELLO or
  a PING) reads and discards whole frames answering a request that timed out, then reads its own. A discarded ERROR
  1, 2, 5 or 6 (after which the server closes the connection) or 7 (the server stopping) ends the connection. A timeout
  in the middle of a frame, or of the send, closes the connection, since the stream can't be resynchronised.
- **The pause.** A connection failure (no connection, a lost or closed connection, a malformed answer, an ERROR that
  ends the connection, an unanswered PING, a HELLO on the main connection unanswered in time or refused, whether in a
  run or from the background thread) or a second RUN timeout in a row closes the connection and starts the pause: every run is then answered at once with NaN, without touching the network, until the background thread's
  HELLO is answered. One log line starts it and one ends it, with how long it lasted and how many runs it answered so.
- **The background thread**, one of the bridge's own, asleep on a condition variable between its timers. It starts
  once a server address is set, never runs on the game's threads, and lives until the game exits.
  - **Reconnecting:** after a failure it connects (3000 ms) and says HELLO with every live model (60 seconds) on a
    new connection, outside the main connection's lock, and swaps it in. It tries after 1, 2, 5, 10 and 30 seconds,
    then every 30 seconds; its answered HELLO ends the pause. It also makes the main connection when there is none
    and a model is live, and asks again, on the same schedule, about a model the server's last answer didn't have
    ready (whose runs meanwhile get the NaN answer at once).
  - **HELLO after a change:** when the live models changed, HELLO with the full list on the open main connection.
  - **PING** on a main connection idle for 60 seconds whose HELLO was answered and listed models. PONG must come
    within the run timeout; a missing one closes the connection and starts the pause and the reconnect.
  - **Status checks:** every 5 minutes, only when the counters changed since the last one, HELLO with no models (the
    counters in its client string) on a connection of its own, then closed. Also once when a server address is set, and
    within seconds of a line falling back (at most one every 30 seconds), with the fallback's reason in the client
    string (`fallback REASON`, the latest when several came meanwhile).
  - **Never past a run's timeout:** its connect and the 60-second HELLO of a reconnect go on a new connection, outside
    the main connection's lock. Its HELLO and PING on the main connection take the lock only when it is free and no
    run waits, and last at most the run timeout. A run that arrives meanwhile waits for the lock within its own
    timeout, which counts from its call (above): it is answered by the server, or gets the NaN answer, by then.
  - While the bridge's own side load is in flight it holds its HELLOs (see the server's duties at loading); status
    checks aren't held. A small gate keeps the two apart: a model load waits for a HELLO already in flight on the main
    connection before its side HELLO goes, so no HELLO of the main connection is caught waiting for the load.
- **Before the settings are read**: SkyrimNet registers the bridge's settings during its own start-up, so the bridge
  reads them at the data-loaded message, when the main menu opens, before each game loads and when its page is
  saved, and counts them read once SkyrimNet names the config. A model load before that is refused ("the bridge's
  settings weren't read yet"; BFA tries a failed load again 30 seconds later) while SkyrimNet is loaded, and as having
  no server address when SkyrimNet isn't.
- **The server's name** is looked up once per change of the address or port, off the game's threads, and connections
  use the addresses found. A failed lookup is logged once, and runs get the NaN answer until the settings change.
- **Every other entry that takes a session** (the 22 of `OrtApi` besides `Run`, such as `SessionGetInputCount`,
  `SessionGetOutputName`, `CreateIoBinding`, `RunWithBinding` and `RunAsync`, and the model editor's three) answers a
  bridge session with an error status made by `CreateStatus`, without the runtime. BFA calls none of them.
- **Releasing** a bridge session frees it in the bridge; it never reaches the runtime. A session the runtime made
  itself (through another entry, such as `CreateSessionFromArray`) is the runtime's own, and every call on it is the
  real one.
- **Everything else** BFA calls (options, providers, tensors, memory information, statuses) is the real runtime's. In
  GPU mode BFA still opens its own Direct3D 12 device before creating a session (its own code, its own setting), but
  no inference runs on it; the bridge logs that BFA is set to GPU and, once per session, shows "BFA Remote Bridge: BFA
  is set to GPU, so it keeps a graphics device open on this PC. Tick Force CPU in BFA's menu: its model runs on the
  server either way." BFA's CPU route sets no free-dimension overrides, so its HELLO lists none.
- **The monitor** (`src/core/Monitor.h`) shows what the server does, from each model load's HELLO_OK and each
  HELLO_OK on the main connection: one line per model with its card, server and state, when it changes and at each
  game load (ready lines only while Status on load allows them), a model load the server refuses included, with its
  detail; a note when lines fall back, naming the reason, at most one a minute however the lines alternate; one when
  lines are back on the server after such a note, at most one a minute. Every server text shown or logged goes on one line
  and loses any path and memory address (`ServerText`), and is cut at about 160 characters on screen
  (`ShownServerText`); the log keeps the whole line. Every note goes through one
  function (the plugin's `ShowNote`): Skyrim's corner note today, which SkyrimNet sees as a `notification` event.
- **The log**, `BFARemoteBridge.log` in SKSE's log folder, times in UTC: BFA found or not, its version, the attachment,
  the settings (the address and the key only as set or not), each model's size, SHA-256 and overrides, the server's
  answer to each model load and to the main connection's HELLO when it changes, the pause's start and end, and **each
  run the server didn't answer**: its model, its reason (no connection, connect timed out, RUN timed out, ERROR with
  its code and text, not ready, connection lost, other) and the time it took. Runs answered at once without the
  network (during a pause, a model not ready, no address or an address not looked up, a replaced session, an input
  the protocol can't carry) are counted by reason; only the first of a series is logged. A model the server keeps not
  ready is logged again only when its state changes, however often the background thread asks. **The summary line**
  counts every run, answered or not, by reason, at each 250 runs and every 10 minutes when anything happened: "250
  runs since the last summary: 249 answered, 1 not (1 timed out, 0 refused, 0 not ready, 0 no connection, 0 paused, 0
  other); round trip 8.1 ms, server 6.2 ms, slowest 21.0 ms". Log detail "Detailed" adds a line per run, each connect
  and close with its reason (a background reconnect's connect is logged at the normal level only when it ends a
  pause), each HELLO's answer, each unanswered PING, each discarded late answer and each background attempt.
