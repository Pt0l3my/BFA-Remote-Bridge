# Changelog

## 1.1.0

Protocol version 1 is unchanged. The scope was agreed with PandorumLLM's side: [docs/AGREEMENT-1.1.md](docs/AGREEMENT-1.1.md).

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
- The settings page's Timeout and Log detail texts describe the new behaviour.
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
