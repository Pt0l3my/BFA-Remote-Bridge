# Changelog

## 1.4.0

Lip sync in scenes. Protocol versions 1 and 2 are unchanged, and the server isn't involved.

- **The scene answer, for BFA only.** Better Facial Animations 4.3.2 leaves the face of anyone in a SexLab or OStim
  scene to the scene: its scene check asks the actor's virtual `Actor::IsInFaction` about SexLab.esm 0xE50F
  (SexLabAnimatingFaction), OStim.esp 0xECA (OStimActorCountFaction) and OStim.esp 0xD93 (OStimExcitementFaction), from
  seven places (the provider's actor check, which gives SkyrimNet "Declined/InScene", a line's start, the face's
  update every frame, its morphs and its gaze among them). At the data-loaded message the bridge finds the three
  factions by plugin and local ID (ignoring the plugin name's case, light plugins included; never by EditorID; a
  plugin that isn't loaded resolves to nothing) and redirects slot 0xF9 of the main vtables of `Character` and
  `PlayerCharacter` (CommonLibSSE-NG's `VTABLE_Character` and `VTABLE_PlayerCharacter`; the slot as CommonLib's
  `Actor.h` declares it and its `RelocateVirtual(0x0F9, 0x0FB)` calls it, SE and AE) to a thunk per vtable. The thunk
  answers `false` only while the setting is on, for one of the three factions found, asked from inside
  `BetterFacialAnimations.dll`'s image (the call's return address); every other call goes to the game's function it
  replaced. The decision is `src/core/SceneLipSync.h`, tested on Linux; the hook is `src/plugin/SceneHook.cpp`.
- **Fail safe.** Nothing is installed, and nothing changed, when BFA isn't loaded, its image's range can't be read
  from its headers, none of the factions is loaded, a slot isn't in the game's image or doesn't hold a function in the
  game's code (another mod redirected it first), or a vtable can't be written (a first one already written is then put
  back). One log line says which. The thunks change an answer only once both vtables are written (an atomic
  "installed" flag): should the first also fail to be put back, its thunk stays and passes every call on, and the log
  line says exactly that. If the settings can't be applied at the data-loaded message, one log line says so and the
  hook isn't installed.
- **The setting:** "Lip sync in scenes" (`sceneLipSync`, on by default) on SkyrimNet's page, under Lip sync. It takes
  effect at once, on every read of the page: when off, the thunk passes every call through, and BFA's next check
  hands a face in a scene back to the scene. It touches nothing of the client's (no reconnection).
- **Logs.** One line at load: the hook installed, with the factions found (plugin, local ID, form ID) and those not
  loaded, or why it wasn't. At Log detail Detailed only, one line for a line kept, at most one a minute, with the
  speaker's form ID, the faction and the lines kept since the last such log line. BFA's per-frame checks are never
  logged: a call is told by its site (the return address into the scene check's caller, from a stack capture taken
  only at Detailed) and its actor. The same site asking about the same actor again within 5 ms is the same check
  asking about the actor's next faction (OStim puts everyone in a scene in both of its), and counts for nothing; a site
  asked about the same actor again within 250 ms is a per-frame one.
- **PE:** `pe::ImageSize` reads a loaded module's SizeOfImage from its headers (MZ, PE signature, PE32+), for BFA's
  and the game's ranges.
- **Address Library** is now used for the two vtables (it was already required).
- **Tests:** `tests/SceneTests.cpp`: each faction, other factions, callers inside and outside BFA's range, the
  setting off, missing factions, form IDs in full and light plugins, plugin names (a longer name that starts with the
  plugin's among them), the slot check, the call's site, the Detailed log's lines (an actor in both of OStim's
  factions among them) and texts, the setting, and the image size (a wrong PE signature among them; BFA's own DLL too,
  with `BFARB_BFA_DLL`).

## 1.3.0

Protocol version 2, the agreement's stage 2 ([docs/AGREEMENT-1.2.md](docs/AGREEMENT-1.2.md)), written first in
[docs/PROTOCOL.md](docs/PROTOCOL.md). 1.3.0 also carries every change of 1.2.0, below, which wasn't released on its
own: players go from 1.1.1 straight to 1.3.0.

- **The version rule.** Every connection (the speech connection, each model load's and each status check's) opens
  with a version 2 HELLO. A version 1 server (PandorumLLM v3.97 Beta patch7 and earlier) answers ERROR 6 framed in version 1;
  the bridge then connects again at once in version 1, within a run's own deadline when a run made the connection, and
  stays on version 1 for that server until the game restarts or the address changes, logged once. A version 2 HELLO
  that gets no frame back (the connection reset or closed before any answer, as p6 and earlier can cause on Windows by
  closing with the HELLO unread) is said once more in version 2; no frame again, and the server is taken as version
  1. The first PandorumLLM to speak version 2 is a release after v3.97 Beta patch7. Nothing of version
  2 goes to a server that hasn't answered in version 2, and an answer framed in a version the bridge didn't open with
  is malformed. HELLO and HELLO_OK carry no field beyond version 1's: only the frame's version says 2.
- **Finding the server: FIND and FOUND on UDP.** With the address setting empty, the bridge sends FIND (the client
  string) to the Port setting on each IPv4 adapter's own broadcast address, from the adapter list Windows reports,
  never to 255.255.255.255; at start-up and after a lost connection, at most once every 5 seconds while no server
  answers. It waits up to a second, takes the first FOUND, and connects on the port FIND reached. A run never
  searches. A model load that finds no server is refused, with a note once per session saying what to set in
  PandorumLLM and to allow UDP as well as TCP on the port. The log names the server found by its own name and port,
  never its address. The Windows part (the adapter list and the UDP socket) is `src/plugin/WinsockDiscovery.cpp`,
  behind a seam like the TCP transport's, so the core and its tests stay on Linux.
- **NOTICE.** To a server that answered in version 2, each line that falls back is told on the speech connection by
  the background thread: the model, the reason's word, the detail (on one line, as shown on screen) and the lines
  since the last NOTICE; at most one a minute, as the fallback note, and only while no request there awaits an
  answer. A version 1 server is told by 1.2.0's status check with `fallback REASON` instead.
- **No hardware advice.** Every hardware choice is PandorumLLM's, BFA's own Force CPU included, and neither side
  advises on BFA's GPU or CPU setting: the on-screen note about BFA's GPU mode, its code and tests are gone, and BFA's GPU mode is logged once, as a fact, at Log detail Detailed only.
- **BFA's mode, set by the bridge.** With a PandorumLLM server that speaks protocol version 2, the bridge switches BFA
  to its CPU mode itself, from the next game start, so BFA opens nothing on this PC's graphics card; the model still
  runs on the server. Once a server has answered in version 2 this session, it writes `[General] ForceCPU=1` in
  BFA's `BetterFacialAnimations.ini` (the relative path BFA uses), and again at each game save (SKSE's save message),
  so a save of BFA's menu in the session can't undo it. When it changes a 0, it notes that it did in its own
  `SKSE/Plugins/BFARemoteBridge.ini`; a server that answers only in version 1 (PandorumLLM v3.97 Beta patch7 and earlier)
  gets the 0 put back. A note that is missing or unreadable puts nothing back, so the bridge's own 1 is never taken
  for the player's value. Only for BFA 4.3.2.3, whose sizes version 2 servers carry: another BFA version's mode is
  left as it is, and a value the bridge set put back. BFA's INI is never created: without it, nothing is written. One
  log line, no setting, no note. The rule is `src/core/BfaMode.h`, the files `src/plugin/BfaIni.cpp`, behind an IniStore
  seam. If you remove the bridge, BFA keeps the CPU mode the bridge set; BFA's own menu shows it.
- **The sizes rule** ([docs/AGREEMENT-1.2.md](docs/AGREEMENT-1.2.md), measurement 5): every version 2 server, given
  a HELLO that lists no overrides for a file of BFA 4.3.2.3's set (by SHA-256), fills in that set's sizes and says so
  in the row's detail, so BFA's CPU mode is served (PandorumLLM's patch7 does the same in version 1). The bridge
  tells a server's version by its frames alone, never by a detail's words, so it sets BFA's CPU mode only for a
  version 2 server.
- **The settings page:** the Server address and Port texts say that an empty address means the server is found on
  the local network.
- **The reference server** speaks both versions (`--v1-only` for version 1 alone), logs NOTICE (`--notice-log`),
  answers FIND from its `--remote-ip` alone, at most once a second, and fills in sizes for its test files when a HELLO
  lists none (`--fill-sizes`), so a test shows that BFA's mode follows the version, not the detail. The tests run
  the bridge against it in both versions, a version 1 server refusing the version 2 HELLO, and find it on the
  loopback address.

## 1.2.0

Not released on its own: these changes ship in 1.3.0.

Protocol version 1 is unchanged; it works with PandorumLLM v3.97-p6. What needs the server's side is agreed with it,
in two stages: [docs/AGREEMENT-1.2.md](docs/AGREEMENT-1.2.md).

- **Not an alternative to BFA: a bridge to PandorumLLM's A2F server.** The README says so at the top, says that BFA
  stays installed and how to give PandorumLLM's PC the same BFA files.
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
