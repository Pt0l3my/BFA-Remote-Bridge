# BFA Remote Bridge

An SKSE plugin for Skyrim Special Edition and Anniversary Edition that runs
Better Facial Animations (BFA)'s lip-sync model on another PC
of your local network instead of the one playing the game.

BFA runs NVIDIA's Audio2Face models through ONNX Runtime on the gaming PC's graphics card every time an NPC speaks.
When that card is already busy drawing the game, each burst of inference pushes frames back and the game stutters.
The bridge sends BFA's model runs to an **A2F server** on another PC (PandorumLLM's, or any server that speaks
[the bridge's protocol](docs/PROTOCOL.md)) and hands the results back to BFA, so the arithmetic happens there.

BFA's files are never changed: the bridge attaches to BFA in memory when the game starts.

## How it works

- At start-up the bridge finds `BetterFacialAnimations.dll` and redirects its one lookup of ONNX Runtime's entry
  point, so BFA's calls to the runtime pass through the bridge.
- **While the bridge is on, no BFA model runs on the gaming PC**, neither on its graphics card nor on its
  processor. When BFA loads a model, the bridge asks the server to load the same file (matched by name, size and
  SHA-256); every run then goes to the server, BFA's warm-up run included.
- **There is no local fallback.** A late lip sync looks worse than none, and running the model on the processor
  stutters too:
  - If the server can't load the model (it isn't running, the key is wrong, it lacks the file or has another copy),
    BFA's model isn't loaded, and lip sync is the game's own. Start the server, then reload the model in BFA's menu
    or restart the game.
  - If the server doesn't answer a call in time, that one line gets the game's own lip sync, BFA keeps its facial
    model, and the next line goes to the server again on the same connection. Its sentiment model is the exception:
    BFA drops it after a missed answer and loads it again 30 seconds later (60 in GPU mode).
  - If the connection fails (the server stops, the network drops, or two calls in a row time out), the bridge pauses:
    lines get the game's own lip sync at once, without waiting, while the bridge reconnects in the background (after
    1, 2, 5, 10 and 30 seconds, then every 30 seconds). Speech goes back to the server the moment it answers again.
- The bridge keeps one connection for speech. When BFA loads a model, the server loads it on a short-lived connection
  of its own, so the lines of a model already loaded keep flowing meanwhile. After a minute without speech the bridge
  checks the connection (PING), so it is ready before the next line needs it.
- Turning the bridge on or off takes effect the next time BFA loads its model: restart the game, or switch the model
  in BFA's menu. A model BFA loaded while the bridge was off runs on this PC as usual.

## Requirements

- Skyrim Special Edition 1.5.97 or Anniversary Edition 1.6.x (1.6.1170 and later included). Not VR.
- [SKSE](https://skse.silverlock.org/) for your game version and
  [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444).
- SkyrimNet Beta26 or later: the bridge's settings are on its
  Settings > Plugins page.
- Better Facial Animations 4.3.2 (it ships ONNX Runtime 1.24.4, the version the bridge knows). The bridge attaches
  only to BFA 4.3.x; with any other version, or another runtime, it passes everything through and BFA runs as usual.
- An A2F server on another PC of your local network, holding the same model files as your BFA folder, such as
  PandorumLLM's A2F server.

## Install (Mod Organizer 2)

1. Download `BFARemoteBridge-<version>.zip` and install it with **Install a new mod from archive**. It holds
   `SKSE/Plugins/BFARemoteBridge.dll` and SkyrimNet's settings page for it,
   `SKSE/Plugins/SkyrimNet/external/pt0l3my.bfaremotebridge/`.
2. Enable it, below Better Facial Animations or anywhere: the order doesn't matter.
3. On the server's PC, set up the A2F server with a copy of your `BetterFacialAnimations` folder's `.onnx` files.
4. In PandorumLLM's panel, set **Remote IP** to the gaming PC's address on your network. Until it is set, the A2F
   server listens on its own PC only (127.0.0.1) and refuses the gaming PC; once set, it listens on the network and
   admits only that address, its own PC and 127.0.0.1.
5. In the game, open SkyrimNet's **Settings > Plugins** page, find **BFA Remote Bridge**, enter the server's address,
   save, and restart the game. PandorumLLM's A2F server doesn't use an access key yet, so leave that empty (any key is
   accepted).

By hand: copy the zip's `SKSE` folder into the game's `Data` folder.

## Set BFA to Force CPU

**Tick Force CPU in BFA's menu when you use the bridge.**

- With Force CPU, the gaming PC's graphics card does no BFA work at all.
- In BFA's GPU mode, the bridge still sends every run to the server, and no lip-sync inference runs on the gaming
  card; but BFA still opens a Direct3D 12 device of its own on that card before it loads a model. Force CPU avoids
  even that. The bridge notes in its log when BFA is set to GPU, and says so once on screen: "BFA Remote Bridge: BFA
  is set to GPU. Its Force CPU setting keeps it off this PC's graphics card."
- In GPU mode BFA also reacts to a refused model load or a failed warm-up as to a failing card: with its independent
  device it turns its GPU route off until you press "Try GPU again" in its menu, and with the shared device it may
  turn DirectML's metacommands off for the session. Another reason for Force CPU.

## Settings

On SkyrimNet's Settings > Plugins page, under **BFA Remote Bridge**:

| Setting | Default | What it does |
|---|---|---|
| Bridge | on | Runs BFA's lip-sync model on the server, and never on this PC. Takes effect at BFA's next model load. Off, BFA runs as if the bridge weren't installed. |
| Server address | (empty) | The address of the PC running the A2F server, on your local network. Empty keeps the bridge idle, and BFA runs as usual. |
| Port | 7871 | The A2F server's port, as set in PandorumLLM. |
| Access key | (empty) | The key set in PandorumLLM, if any. |
| Timeout (ms) | 1000 | How long each lip-sync call waits for the server (200 to 5000). A line it doesn't answer in time gets the game's own lip sync. If the connection fails, lines get the game's own lip sync until the bridge has reconnected in the background. |
| Check results | 0 | A diagnostic: also loads BFA's model on this PC and runs this many of each model's first calls on both PCs, logging how far apart the results are. While it is above 0 the model runs on this PC too, so keep it at 0 for play. Small differences are expected when the server runs on another device (DirectML against the processor), uses another thread count, or runs another model in a role's place (the log's HELLO line says so); identical results need the same file, the same device and the same thread count. |
| Status on load | on | Shows the bridge's state on screen when a save loads and when BFA loads its model, and once per session if BFA is set to GPU. |
| Log detail | Normal | Detailed also logs every call's round trip, each connection opened or closed and why, each answer to HELLO, each unanswered PING and each reconnection attempt. |

The bridge reads the settings when the game starts, when the main menu opens, before each save loads, and whenever
you save the page. Until SkyrimNet has registered them, BFA's model loads are refused (BFA tries again 30 seconds
later), so that the model never runs on this PC by mistake.

## The log

`Documents/My Games/Skyrim Special Edition/SKSE/BFARemoteBridge.log` (times in UTC). It says:

- whether BFA was found, its version, and whether the bridge attached to it;
- the settings (the server address and the access key only as set or not);
- the server's first answer at start-up (its name and device, such as "PandorumLLM ... A2F" on "DirectML 0: ..." or
  "CPU: ...");
- each model BFA loads: its size, SHA-256 and overrides, and whether the server serves it ("is served by ...;
  nothing of it runs on this PC") or why not, with the server's note when it runs another model in that role's place;
- each model BFA loads on this PC instead, and why (the bridge is off, no address, SkyrimNet isn't loaded);
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
to HELLO, each unanswered PING, each late answer discarded and each background reconnection attempt.

The bridge also tells the server how it is doing: its HELLO carries its counters since the game started, as
"BFARemoteBridge 1.1.0 (answered 1250, fallbacks 0, timeouts 0)", and every 5 minutes, when they changed, it sends
them on a short connection of its own, so PandorumLLM's panel shows them.

## Troubleshooting

| You see | What to do |
|---|---|
| "Better Facial Animations wasn't found, so the bridge is idle." | Install and enable BFA. |
| "no server address is set" | Enter the server's address on SkyrimNet's page, save, and restart. |
| "SkyrimNet wasn't found" | Install SkyrimNet Beta26 or later: the settings are read from it. |
| "the server didn't answer" | Start the A2F server, check the address and port, and allow the port through the server PC's firewall (private networks only). Then reload the model in BFA's menu, or restart. |
| "the server refused the access key" | Enter the same key as the server's. |
| "the server lacks ... or has a different copy" | Copy the model files from your `BetterFacialAnimations` folder to the server's model folder again: they must be identical. |
| "couldn't attach to it" | Another mod loaded BFA's ONNX Runtime first, or this BFA differs from 4.3.2: the log says which. BFA runs as usual. |
| Lips still on the game's own animation now and then | The log's "not answered" lines and summaries say why; raise the timeout if the server is slow. |
| "BFA is set to GPU" | Tick Force CPU in BFA's menu (see above). |

## Building

- **Linux** (the core and its tests): `cmake -S . -B build -DBFARB_PYTHON=<python with onnxruntime==1.24.4, onnx and
  numpy>`, `cmake --build build`, `ctest --test-dir build --output-on-failure`. The tests run the real runtime against
  `tools/reference_server.py`.
- **Windows** (the DLL): from a Visual Studio developer prompt with `VCPKG_ROOT` set, `cmake --preset windows` and
  `cmake --build --preset windows`.
- `sh tools/wincheck/fetch.sh` then `sh tools/wincheck/wincheck.sh` parse the Windows files on Linux with clang.

## Licences

- BFA Remote Bridge: MIT (`LICENSE`), copyright Pt0l3my.
- The vendored ONNX Runtime C API headers (`third_party/onnxruntime/`): MIT, Microsoft Corporation.
- CommonLibSSE-NG, spdlog and fmt, linked into the DLL: MIT. See `THIRD-PARTY-NOTICES.md`.
- No file of Better Facial Animations (GPL-3.0), of NVIDIA (the Audio2Face and Audio2Emotion models), of ONNX
  Runtime's DLLs or of DirectML is included.
