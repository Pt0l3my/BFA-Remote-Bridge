# BFA Remote Bridge

An SKSE plugin for Skyrim Special Edition and Anniversary Edition that sends Better Facial Animations (BFA)'s
lip-sync model runs to **PandorumLLM's A2F server** on another PC of your local network, and shows on screen what the
server does.

**This mod is not an alternative to Better Facial Animations.** It is a bridge between BFA and PandorumLLM's A2F
server:

- **BFA stays installed on the gaming PC.** Its game side (catching each line, reading the audio, fitting the face
  and moving it every frame) and its settings stay there. Tick **Force CPU** in BFA's menu (see below).
- **PandorumLLM's PC needs BFA's files too**: the server runs BFA's models there, so it needs the same models and
  runtime as your BFA folder. See [On PandorumLLM's PC](#on-pandorumllms-pc).
- **What still runs on the gaming PC:** BFA's game side only. The model runs on the server, on the card PandorumLLM
  is set to use; PandorumLLM chooses the hardware, and the bridge only reports it.

BFA runs NVIDIA's Audio2Face models through ONNX Runtime every time an NPC speaks. When the gaming PC's graphics card
is already busy drawing the game, each burst of inference pushes frames back and the game stutters. With the bridge,
the arithmetic happens on the server's PC, and BFA gets the results back. BFA's files are never changed: the bridge
attaches to BFA in memory when the game starts.

## How it works

- At start-up the bridge finds `BetterFacialAnimations.dll` and redirects its one lookup of ONNX Runtime's entry
  point, so BFA's calls to the runtime pass through the bridge. That is the only thing it changes of BFA's.
- **With the bridge, no BFA model runs on the gaming PC**, neither on its graphics card nor on its processor. When BFA
  loads a model, the bridge asks the server to load the same file (matched by name, size and SHA-256); every run then
  goes to the server, BFA's warm-up run included.
- **There is no local fallback.** A late lip sync looks worse than none, and running the model on the processor
  stutters too. The runtime's other ways to make a session (from memory, with prepacked weights, the model editor's)
  are refused with an error naming them, since a model loaded so would run on this PC:
  - With no server address set, BFA's model isn't loaded and lip sync is the game's own; a note says so once.
  - If the server can't load the model (it isn't running, it lacks the file or has another copy), BFA's model isn't
    loaded, and lip sync is the game's own. Start the server, then reload the model in BFA's menu or restart the game.
  - If the server doesn't answer a call in time (1000 ms), that one line gets the game's own lip sync, BFA keeps its
    facial model, and the next line goes to the server again on the same connection. Its sentiment model is the
    exception: BFA drops it after a missed answer and loads it again 30 seconds later (60 in GPU mode).
  - If the connection fails (the server stops, the network drops, or two calls in a row time out), the bridge pauses:
    lines get the game's own lip sync at once, without waiting, while the bridge reconnects in the background (after
    1, 2, 5, 10 and 30 seconds, then every 30 seconds). Speech goes back to the server the moment it answers again.
- The bridge keeps one connection for speech. When BFA loads a model, the server loads it on a short-lived connection
  of its own, so the lines of a model already loaded keep flowing meanwhile. After a minute without speech the bridge
  checks the connection (PING), so it is ready before the next line needs it.
- **Any BFA version is tried.** The bridge finds out what doesn't work by what happens, not by a version number: it
  serves the ONNX Runtime API version BFA asks for, older or newer than the 1.24.4 it knows (an entry newer than
  1.24.4's answers with an error, so whatever needs it gets the game's own lip sync, and the log names it), and the
  server matches the model files. A runtime the bridge can't serve (older than 1.10, or newer than it serves) is
  known to be incompatible: BFA's model isn't loaded, lip sync is the game's own, and a note says what to update. If
  the bridge can't attach to BFA at all, it says so on screen, and BFA runs on this PC as it would without the bridge:
  update the bridge.

## Requirements

- Skyrim Special Edition 1.5.97 or Anniversary Edition 1.6.x (1.6.1170 and later included). Not VR.
- [SKSE](https://skse.silverlock.org/) for your game version and
  [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444).
- SkyrimNet Beta26 or later: the bridge's settings are on its Settings > Plugins page.
- Better Facial Animations (tested with 4.3.2, which ships ONNX Runtime 1.24.4). Other versions are tried too.
- PandorumLLM's A2F server on another PC of your local network, holding the same BFA files as your gaming PC (or any
  server that speaks [the bridge's protocol](docs/PROTOCOL.md)).

## Install (Mod Organizer 2)

1. Download `BFARemoteBridge-<version>.zip` and install it with **Install a new mod from archive**. It holds
   `SKSE/Plugins/BFARemoteBridge.dll` and SkyrimNet's settings page for it,
   `SKSE/Plugins/SkyrimNet/external/pt0l3my.bfaremotebridge/`.
2. Enable it, below Better Facial Animations or anywhere: the order doesn't matter. Keep BFA enabled.
3. Set up PandorumLLM's PC (below).
4. In the game, open SkyrimNet's **Settings > Plugins** page, find **BFA Remote Bridge**, enter the address of
   PandorumLLM's PC, save, and restart the game.

By hand: copy the zip's `SKSE` folder into the game's `Data` folder.

To run BFA on its own again, disable the bridge in Mod Organizer 2.

## On PandorumLLM's PC

The server runs BFA's models, so it needs BFA's files, of the **same BFA version** as the gaming PC:

1. Copy the gaming PC's `Data/SKSE/Plugins/BetterFacialAnimations/` folder to PandorumLLM's PC (or install the same
   BFA version there and take that folder). It must hold:
   - the `.onnx` models (`a2f3_fp16.onnx`, `emotion_fp16.onnx`, `performance.onnx`, `network_fp16.onnx`, as many as
     your BFA has);
   - `onnxruntime.dll`, `DirectML.dll` and `onnxruntime_providers_shared.dll`;
   - BFA's licence files (`NOTICE.txt` and the `licenses` folder), which go with its models.
2. In PandorumLLM's panel, point the A2F server's **Model folder** at that folder, and choose the card it runs on.
3. Set **Remote IP** to the gaming PC's address on your network. Until it is set, the A2F server listens on its own
   PC only (127.0.0.1) and refuses the gaming PC; once set, it listens on the network and admits only that address,
   its own PC and 127.0.0.1. Allow the port (7871) through that PC's firewall, for private networks only.
4. **Keep both copies the same version.** After updating BFA on the gaming PC, copy its folder again: a model that
   differs from the server's copy (by size or SHA-256) is refused, and its lines get the game's own lip sync.

## Set BFA to Force CPU

**Tick Force CPU in BFA's menu once.** It costs nothing: with the bridge, the model runs on the server either way.

- BFA's GPU mode is BFA's own setting, and the bridge doesn't change it. In GPU mode BFA opens a graphics device of
  its own on the gaming PC's card when it starts (idle afterwards, since the runs go to the server). Force CPU avoids
  that device.
- The bridge notices BFA's GPU mode and says so once per session: "BFA Remote Bridge: BFA is set to GPU, so it keeps
  a graphics device open on this PC. Tick Force CPU in BFA's menu: its model runs on the server either way."
- In GPU mode BFA also reacts to a refused model load or a failed warm-up as to a failing card: with its independent
  device it turns its GPU route off until you press "Try GPU again" in its menu. Another reason for Force CPU.
- BFA's CPU route sets none of the fixed input sizes its GPU route gives the runtime, so the server builds its
  sessions without them. Both sides measure one session with Force CPU before deciding whether the server should set
  them ([docs/AGREEMENT-1.2.md](docs/AGREEMENT-1.2.md), "To measure").

## Settings

On SkyrimNet's Settings > Plugins page, under **BFA Remote Bridge**. The page holds only what the server can't tell
the bridge: the hardware the model runs on is chosen in PandorumLLM.

| Setting | Default | What it does |
|---|---|---|
| Server address | (empty) | The address of PandorumLLM's PC on your local network. Empty means no server: BFA's model isn't loaded, lip sync is the game's own, and a note says so once. Finding PandorumLLM's PC by itself comes with bridge 1.3.0 ([docs/AGREEMENT-1.2.md](docs/AGREEMENT-1.2.md)). |
| Port | 7871 | The A2F server's port, as set in PandorumLLM. Used with the address above. |
| Status on load | on | Shows on screen, at each game load and when a model is served, which card and server run each of BFA's models. Notes about lines getting the game's own lip sync show either way. |
| Log detail | Normal | Detailed also logs every call's round trip, each connection opened or closed and why, each answer to HELLO, each unanswered PING and each reconnection attempt. |

Bridge (on or off), Access key, Timeout and Check results, the settings of 1.1, are gone: with the bridge installed it
takes over, PandorumLLM admits the gaming PC by its Remote IP, each call waits 1000 ms, and nothing of the model runs
on this PC to check against. Values saved for them are ignored.

The bridge reads the settings when the game starts, when the main menu opens, before each save loads, and whenever
you save the page. Until SkyrimNet has registered them, BFA's model loads are refused (BFA tries again 30 seconds
later), so that the model never runs on this PC by mistake.

## The monitor

The bridge tells you on screen what the server does, one line per model, from the server's own answers:

- when a model is served, and at each game load (while **Status on load** is on): "BFA Remote Bridge: a2f3_fp16.onnx
  on NVIDIA GeForce RTX 5090 (DirectML 3), PandorumLLM v3.97-p6 Beta A2F: ready";
- whenever a model's card, server or state changes, such as a model the server no longer has ready, or one it
  refuses at a load, with the server's detail: "BFA Remote Bridge: a2f3_fp16.onnx on NVIDIA GeForce RTX 5090 (DirectML
  3), PandorumLLM v3.97-p6 Beta A2F: not ready (different file) (...)";
- when lines fall back to the game's own lip sync, with the reason, at most once a minute however the lines
  alternate: "BFA Remote Bridge: a2f3_fp16.onnx: lines get the game's own lip sync, since the server didn't answer in
  time.";
- when lines are back on the server after such a note, at most once a minute: "BFA Remote Bridge: a2f3_fp16.onnx:
  lines are back on the server." (without the model when another model's line came back);
- once per session: no server address set, BFA set to GPU, the bridge unable to attach, or a runtime it can't serve.

A server's detail (such as ONNX Runtime's message for a model that failed to load) is put on one line, loses any
folder and memory address, and is cut at about 160 characters on screen; the log keeps the whole line.

Every note begins with "BFA Remote Bridge:" and uses plain words (Skyrim's fonts can't draw emoji). The notes are
Skyrim's own corner notifications, so SkyrimNet sees each one as a `notification` event: a SkyrimNet workflow trigger
can react to them by that beginning.

The server is told too: when a line falls back, the bridge sends its status check within seconds (at most one every
30 seconds) with the reason in its client string, so PandorumLLM's log shows it.

## The log

`Documents/My Games/Skyrim Special Edition/SKSE/BFARemoteBridge.log` (times in UTC). It says:

- whether BFA was found, its version, and whether the bridge attached to it, or why not;
- the ONNX Runtime BFA loaded, and any runtime or API version the bridge can't serve, or any newer entry BFA called;
- the settings (the server address only as set or not);
- the server's first answer at start-up (its name and device, such as "PandorumLLM ... A2F" on "DirectML 0: ..." or
  "CPU: ...");
- each model BFA loads: its size, SHA-256 and overrides, and whether the server serves it ("is served by ...;
  nothing of it runs on this PC") or why not, with the server's note when it runs another model in that role's place;
  a refusal BFA meets again on its retries is logged once;
- each line the server didn't answer: the model, the reason (no connection, connect timed out, RUN timed out, ERROR
  with its code and text, not ready, connection lost) and the time it took, as "a2f3_fp16.onnx: not answered (RUN
  timed out: no answer within 1000 ms) after 1001.2 ms; its line gets the game's own lip sync". Lines answered at
  once during a pause are counted, not logged one by one;
- each pause, when it starts ("...; lines get the game's own lip sync until the server answers again") and when it
  ends, with how long it lasted;
- every 250 runs, and every 10 minutes when anything happened, a summary of every run by reason, with the average
  round trip, the server's own time and the slowest call:
  "250 runs since the last summary: 249 answered, 1 not (1 timed out, 0 refused, 0 not ready, 0 no connection, 0
  paused, 0 other); round trip 8.1 ms, server 6.2 ms, slowest 21.0 ms".

With Log detail set to Detailed it also logs one line per run, each connection opened or closed and why, each answer
to HELLO, each unanswered PING, each late answer discarded, each background reconnection attempt and each status
check sent for a fallback.

The bridge's HELLO carries its counters since the game started and the gaming PC's versions, as "BFARemoteBridge 1.2.0
(answered 1250, fallbacks 0, timeouts 0, BFA 4.3.2.3, ORT 1.24.4)", and every 5 minutes, when they changed, it sends
them on a short connection of its own, so PandorumLLM's panel shows them.

## Troubleshooting

| You see | What to do |
|---|---|
| "Better Facial Animations wasn't found, so the bridge is idle." | Install and enable BFA: the bridge works with it, not instead of it. |
| "no server address is set" | Enter the address of PandorumLLM's PC on SkyrimNet's page, save, and restart. |
| "SkyrimNet wasn't found" | Install SkyrimNet Beta26 or later: the settings are read from it. |
| "the server isn't answering" | Start PandorumLLM's A2F server, check the address and port, set its Remote IP to the gaming PC, and allow the port through the server PC's firewall (private networks only). Then reload the model in BFA's menu, or restart. |
| "the server refused the access key" | The bridge has no access key any more: turn the key off in PandorumLLM's A2F settings. |
| "the server lacks ... or has a different copy" | Copy the gaming PC's `BetterFacialAnimations` folder to the server's Model folder again: the two must be the same BFA version. |
| "couldn't attach to Better Facial Animations" | This BFA looks ONNX Runtime up in a way the bridge doesn't know, or another mod loaded it first: the log says which. BFA runs on this PC as usual; update the bridge. |
| "uses ONNX Runtime ..., which the bridge can't serve" | BFA's model isn't loaded and lip sync is the game's own. For a runtime newer than the bridge knows, update the bridge; for one older than 1.10, update BFA. |
| "runtime entry ... which the bridge doesn't know" | This BFA's runtime is newer than the bridge knows: update the bridge. |
| Lips on the game's own animation now and then | The notes and the log's "not answered" lines and summaries say why. |
| "BFA is set to GPU" | Tick Force CPU in BFA's menu (see above). |

## Building

- **Linux** (the core and its tests): `cmake -S . -B build -DBFARB_PYTHON=<python with onnxruntime==1.24.4, onnx and
  numpy>`, `cmake --build build`, `ctest --test-dir build --output-on-failure`. The tests run the real runtime against
  `tools/reference_server.py`, and serve it as older and newer runtimes would.
- **Windows** (the DLL): from a Visual Studio developer prompt with `VCPKG_ROOT` set, `cmake --preset windows` and
  `cmake --build --preset windows`.
- `sh tools/wincheck/fetch.sh` then `sh tools/wincheck/wincheck.sh` parse the Windows files on Linux with clang.
- **Sanitiser builds** (Linux, for development), each in a build folder of its own: `-DBFARB_SANITIZE=ON` (such as
  `build-asan`) adds AddressSanitizer and UndefinedBehaviorSanitizer, and `-DBFARB_TSAN=ON` (such as `build-tsan`)
  ThreadSanitizer. Run them with `ctest` as above. With GCC, the ThreadSanitizer build adds `tests/TsanClocklock.cpp`
  to the test program: GCC 13's ThreadSanitizer doesn't see `std::timed_mutex`'s timed locks and would report their
  unlocks falsely, and that file routes them through a lock it sees. A clean run prints no "WARNING: ThreadSanitizer".

## Licences

- BFA Remote Bridge: MIT (`LICENSE`), copyright Pt0l3my.
- The vendored ONNX Runtime C API headers (`third_party/onnxruntime/`): MIT, Microsoft Corporation.
- CommonLibSSE-NG, spdlog and fmt, linked into the DLL: MIT. See `THIRD-PARTY-NOTICES.md`.
- No file of Better Facial Animations (GPL-3.0), of NVIDIA (the Audio2Face and Audio2Emotion models), of ONNX
  Runtime's DLLs or of DirectML is included. BFA's files go to PandorumLLM's PC from your own BFA install, with its
  licence files.
