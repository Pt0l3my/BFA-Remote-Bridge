# Changelog

## 1.2.0

Protocol version 1 is unchanged; it works with PandorumLLM v3.97-p6. What needs the server's side is agreed with it,
in two stages: [docs/AGREEMENT-1.2.md](docs/AGREEMENT-1.2.md).

- **Not an alternative to BFA: a bridge to PandorumLLM's A2F server.** The README says so at the top, says that BFA
  stays installed (with Force CPU ticked) and how to give PandorumLLM's PC the same BFA files.
- **The settings page keeps only what the server can't tell the bridge:** the server address and port (for a server
  that doesn't answer discovery yet, as today's), Status on load and Log detail. **Bridge**, **Access key**,
  **Timeout** (now fixed at 1000 ms) and **Check results** are gone, with the code that loaded and ran the model on
  this PC for the check. Values saved for them are ignored. With the bridge installed it takes over; to run BFA on its
  own, disable the bridge in Mod Organizer 2.
- **No server address: no model on this PC.** Every model load is refused, lines get the game's own lip sync, and a
  note says so once. Before, BFA then ran its model on this PC.
- **The monitor.** One line per model with the card and the server that run it and its state, at each load (Status
  on load) and whenever they change; a note when lines fall back to the game's own lip sync, naming the reason, at
  most one a minute, however the lines alternate; one when lines are back on the server, at most one a minute. The reasons are one list
  (`src/core/Reasons.h`). Every note begins with "BFA Remote Bridge:" and goes through one function, Skyrim's corner
  note today (which SkyrimNet sees as a `notification` event), ready for SkyrimNet's own notification call.
- **The server is told of fallbacks:** within seconds of one, at most every 30 seconds, a status check carries its
  reason in the client string ("fallback timeout"). The client string also names BFA's version and the runtime's
  ("BFA 4.3.2.3, ORT 1.24.4"), within its 120 characters.
- **Any BFA is tried.** The 4.3.x gate is gone. The ONNX Runtime API version BFA asks for is served from the real
  table: for an older runtime, only as many entries as its version has (a count per API version, checked against the
  header), never reading past its table; for a newer version, 1.24.4's entries wrapped as before and every entry
  past them answered with an error status naming it (GetExperimentalFunction, index 422, with null: its "not
  available"). A runtime the bridge can't serve (older than API version 10, an unreadable version, or an API version
  past 30) is known to be incompatible: BFA gets no table, so it has no model and lip sync is the game's own, and a
  note says to update the bridge (or BFA, for a runtime older than 1.10).
- **BFA's GPU mode is reported, not changed:** once per session, on screen whatever Status on load says, with "Tick
  Force CPU in BFA's menu". The bridge changes nothing else of BFA's.
- A model load BFA retries after a refusal is logged once until the refusal changes. A model the server answers as
  not ready shows on screen with the server's detail. Every server text shown or logged goes on one line (line
  breaks and control characters become spaces), loses any path and memory address, and is cut at about 160
  characters on screen; the log keeps the whole line.
- **The runtime's other session makers are refused:** CreateSessionFromArray, the two with prepacked weights and the
  model editor's three answer with an error status naming them, logged once, since a model loaded so would run on
  this PC.

## 1.1.1

Protocol version 1 is unchanged. Made for PandorumLLM v3.97 Beta patch4.

- **The client string may run to 120 characters,** which PandorumLLM's log lines keep since its patch4 (80 before),
  so its three counters always go whole: 116 characters at most. A panel before patch4 shows the first 80 in its log.
- The docs describe patch4: a HELLO of models already loaded is answered at once, even while another connection
  loads a model. The bridge still holds its background HELLOs while its own model load is in flight: with patch4 that
  costs nothing, and it keeps the bridge right with patch3.

## 1.1.0

Protocol version 1 is unchanged. The scope was agreed with PandorumLLM's side: [docs/AGREEMENT-1.1.md](docs/AGREEMENT-1.1.md).

- **The author is Pt0l3my,** so the SkyrimNet plugin id and folder are `pt0l3my.bfaremotebridge`. Remove the old
  version before installing this one: two folders would show the page twice. The settings keep their values, which
  SkyrimNet saves under the config name, not the folder.

- **Model loads on a side connection.** When BFA loads a model, the bridge asks the server for it on a short-lived
  connection of its own (3000 ms to connect, 60 seconds for the server to load it), then lists every live model on
  the main connection, where the answer comes at once. Lines of a model already loaded keep flowing during a load.
- **One main connection for speech.** A model load no longer closes it.
- **A pause in place of the flat 10-second backoff.** A connection failure, or a second call in a row that times out,
  pauses the bridge: lines get the game's own lip sync at once while a background thread of the bridge's own
  reconnects after 1, 2, 5, 10 and 30 seconds, then every 30 seconds. The pause ends the moment the server answers.
- **A single timeout keeps the connection.** The server answers in order, so the late answer is read and discarded
  before the next one; a late ERROR 1, 2, 5, 6 or 7 ends the connection. A timeout in the middle of an answer closes
  it.
- **A connect timeout of its own:** 3000 ms for connects off the speech path (model loads, status checks, the
  background thread). A connect within a call stays within that call's timeout.
- **A call's timeout counts from the moment BFA makes it,** its wait for the connection included: the background
  thread's PING or HELLO never makes a line wait longer than the timeout.
- **PING** after 60 seconds idle, on a connection whose HELLO listed models; a missing PONG starts the reconnect.
- **The counters in the client string,** "BFARemoteBridge 1.1.0 (answered N, fallbacks N, timeouts N)", within 80
  characters, in every HELLO; a status check on a connection of its own every 5 minutes when they changed.
- **Logging.** Each line the server didn't answer is logged with its model, reason and time (lines answered at once
  during a pause are counted, not logged one by one). The summary line counts every run by reason, at each 250 runs
  and every 10 minutes when anything happened. A reconnect within a call says whether the connection was closed,
  reset or the send failed. With no server address, or one not looked up yet, only the first line of a series is
  logged, and a model the server keeps not ready is logged once until its state changes. Log detail "Detailed" adds
  each connect and close with its reason, each HELLO's answer, each unanswered PING and each background attempt.
- **A notice on screen when BFA is set to GPU,** once per session while the bridge is on and Status on load allows
  notices: "BFA Remote Bridge: BFA is set to GPU. Its Force CPU setting keeps it off this PC's graphics card."
- The settings page's Timeout, Status on load and Log detail texts describe the new behaviour.
- The reference server answers PONG only after HELLO, closes the connection after ERROR 1, 2, 5, 6 or 7, keeps idle
  connections open by default, and has test options for late and failing answers.

## 1.0.0

The first release.

- Attaches to Better Facial Animations 4.3.x in memory and answers its lookup of ONNX Runtime's entry point, so every
  model load and run passes through the bridge; any other BFA or runtime version runs as usual.
- While the bridge is on with a server address, no BFA model runs on the gaming PC: each model load asks the A2F
  server for the same file (name, size and SHA-256) with BFA's overrides, and each run goes to the server.
- A refused model load fails BFA's session creation with the reason; a run the server doesn't answer in time gets
  NaN outputs, so BFA drops that line and the game's own lip sync plays, followed by a 10-second backoff.
- A connection the server closed while idle is opened again within the same call.
- Settings on SkyrimNet's Settings > Plugins page; on-screen status at the first game load and at each model load;
  a check mode comparing the server's results with this PC's.
- Protocol version 1 ([docs/PROTOCOL.md](docs/PROTOCOL.md)) and a reference server (`tools/reference_server.py`).
