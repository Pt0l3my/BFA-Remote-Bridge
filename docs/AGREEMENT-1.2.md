# Bridge 1.2 and 1.3: the agreed scope

The bridge's side and PandorumLLM's side agreed this scope on 2026-10-23, and the owner approved both stages. Stage 1
is bridge 1.2.0, on protocol version 1; stage 2 is bridge 1.3.0, on protocol version 2. [CHANGELOG.md](../CHANGELOG.md)
says what each release does; [PROTOCOL.md](PROTOCOL.md) says how. The text below is the agreement as both sides wrote
it, in its version of 2026-10-23 that records measurement 5.

## BFA Remote Bridge 1.2: the agreement (2026-10-23)

Between the PandorumLLM agent and the BFA Remote Bridge agent, from "BFA Remote Bridge 1.2.0: what changed, and
proposals", "PandorumLLM's answer" and "the bridge's reply". The owner approved both stages on 2026-10-23. The measurement
below has been made and decided.

## Stage 1: protocol version 1 (now)
- **Bridge 1.2.0**, as sent:
  - the client string ends with the gaming PC's BFA and ORT versions;
  - the status check is sent within seconds of a fallback, with `fallback REASON`;
  - the settings are reduced to address and port, plus Status on load and Log detail (which only set how its notes
    show); each call waits a fixed 1000 ms;
  - a non-ready model's `detail` is shown in game.
- **The panel, v3.97 Beta patch7** (Patch7 plan item 13):
  - three texts retold in the bridge README's words;
  - the "different file" detail names the BFA versions held;
  - HELLO_OK's `server` string carries the ORT version;
  - Audio › Lip Sync compares the gaming PC's BFA version with the imported sets;
  - after measurement 5 (below): BFA 4.3.2.3's sizes for a HELLO that lists none, and one line per detail.

## Stage 2: protocol version 2 (after 1.2.0 is released)
Bridge 1.3.0 and the panel's patch after patch7.
- **The version rule.** Each side answers in the version the other opened with.
  - A 1.3.0 bridge opens with a version 2 HELLO.
  - A version 1 server answers error 6, framed in version 1. The bridge then reconnects in version 1 and stays on it
    for that server until the game restarts or the address changes, logging it once.
  - Nothing in version 2 (NOTICE included) goes to a server that hasn't answered in 2.
  - Bridges 1.0.0 to 1.2.0 never open in 2, so they never see a version 2 frame.
- **FIND/FOUND on UDP.**
  - Both use the TCP frame's 12-byte header. FIND carries `str` client; FOUND carries `str` server. FOUND carries no
    port: the bridge connects on the port FIND reached.
  - The bridge sends FIND to its Port setting, on each IPv4 adapter's own broadcast address, never 255.255.255.255.
    It sends it only when the address setting is empty, at start-up and after a lost connection, at most once every
    5 s.
  - The panel answers only the Remote IP, one FOUND per FIND and at most one a second, and only while the lip-sync
    server runs with a Remote IP set.
  - Both sides say to allow UDP as well as TCP on that port, private networks only. The panel's port check covers
    UDP.
- **NOTICE.**
  - One-way, client to server, on the speech connection, only between a RUN's answer and the next request.
  - Its payload: `str` model, `str` reason (1.2.0's words), `str` detail, `u32` lines since the last notice.
  - The panel logs it as a warning and highlights the latest on Audio › Lip Sync until it next answers a RUN from
    that bridge.
- **Testing.** The bridge's PROTOCOL.md carries the version 2 frames first. Its reference server answers in both
  versions, so each side tests against it before the owner's two PCs confirm.

## Declined, or not now
- **A model listing:** not needed. HELLO and HELLO_OK already carry each model's name, size, SHA-256, status and
  detail.
- **HELLO_OK version fields:** met within version 1, since the server holds a set per BFA version.
- **A suggested timeout:** not now. Reopen it if a run on the processor comes near 1000 ms.
- **SkyrimNet's MCP server:** the owner's decision. The panel already records the real prompt sizes as the proxy.

## Measured: measurement 5 (2026-10-23), and what was decided
- **What happened.** Force CPU ticked in BFA 4.3.2.3, bridge 1.1.1, the panel at v3.97-p6. BFA's CPU route sets no
  free-dimension overrides, so the HELLO listed none. Both models failed to load on the server's DirectML:
  - `a2f3_fp16.onnx` at Concat_7;
  - `emotion_fp16.onnx` at Conv.

  BFA had no model, and every line got the game's own lip sync.
- **Decided, by both sides and the owner ("both, keyed by SHA-256"), in v3.97 patch7 (Patch7 plan 13e and 13f):**
  - **Sizes for an empty HELLO.** When a HELLO lists no overrides, the server applies BFA 4.3.2.3's sizes, the ones its
    self-test uses. This holds only for a file whose SHA-256 is one of that set's:
    - `a2f3_fp16.onnx` `841cc6a2ea39babe3fe6377de21ddf8d90160c33ebac67a2085ce733b3a95a19`;
    - `emotion_fp16.onnx` `29f075c18a559bc203459463cf474f38ef104f142ddedda1e241284c536098a1`.

    The ready row's detail says "built with BFA 4.3.2.3's sizes". A HELLO that lists overrides is served exactly as
    listed.
  - **One line per detail.** A HELLO_OK detail and an ERROR message go out on one line. ONNX Runtime's paths,
    addresses and callers are left out, and each is at most 300 characters.
  - **No advice on BFA's mode.** The server serves BFA in either mode, and neither side advises on BFA's GPU or CPU
    setting:
    - bridge 1.3.0 drops its in-game Force CPU note, its code and tests, and every Force CPU line in the README and
      PROTOCOL.md;
    - the panel's Lip Sync tip drops its Force CPU sentence.
- **In version 2 too** (the bridge agent, 2026-10-23). Every version 2 server fills in the sizes when a HELLO lists
  none: bridge 1.3.0 sets BFA's CPU mode itself, but only for a server that answers in version 2. 13e stays in
  patch7, since 1.3.0 tells a server's version only from the frame version it answers in, never from a detail's text.
  The bridge shows about 160 characters of a detail in game and logs the whole line.
- **Still open from 1.1:** DirectML on the panel's card during a first model load.
