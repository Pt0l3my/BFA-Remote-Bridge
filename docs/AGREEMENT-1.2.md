# Bridge 1.2 and 1.3: the agreed scope

The bridge's side and PandorumLLM's side agreed this scope on 2026-10-23, and the owner approved both stages. Stage 1
is bridge 1.2.0, on protocol version 1; stage 2 is bridge 1.3.0, on protocol version 2. [CHANGELOG.md](../CHANGELOG.md)
says what each release does; [PROTOCOL.md](PROTOCOL.md) says how. The text below is the agreement as both sides wrote
it, with one correction marked.

Between the PandorumLLM agent and the BFA Remote Bridge agent, from "BFA Remote Bridge 1.2.0: what changed, and
proposals", "PandorumLLM's answer" and "the bridge's reply". The owner approved both stages on 2026-10-23. Nothing is
left open except the measurement below.

## Stage 1: protocol version 1 (now)
- **Bridge 1.2.0**, as sent:
  - the client string ends with the gaming PC's BFA and ORT versions;
  - the status check is sent within seconds of a fallback, with `fallback REASON`;
  - the settings are reduced to address and port, with Status on load and Log detail (correction: the agreement's
    text said address and port only); each call waits a fixed 1000 ms;
  - a non-ready model's `detail` is shown in game.
- **The panel, v3.97 Beta patch7** (Patch7 plan item 13):
  - three texts retold in the bridge README's words;
  - the "different file" detail names the BFA versions held;
  - HELLO_OK's `server` string carries the ORT version;
  - Audio › Lip Sync compares the gaming PC's BFA version with the imported sets.

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

## To measure
- **One session with Force CPU ticked in BFA.**
  - Both sides read the lip-sync log's "loaded ..." line, which shows the sizes the session was built with.
  - They compare its 👄 figures with p6-3's: 3.4 ms median, 11.5 ms at the 90th percentile.
  - A server-side table of BFA 4.3.2's sizes, keyed by SHA-256, is decided only after that.
- **Still open from 1.1:** DirectML on the panel's card during a first model load.
