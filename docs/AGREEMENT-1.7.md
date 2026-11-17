# Bridge 1.7: protocol version 3, as agreed

The bridge's side and PandorumLLM's side agreed protocol version 3 on 2026-11-16: a line's state kept on the server.
PandorumLLM's side wrote the agreement, the owner sent its text to the bridge, and the bridge confirmed it on
2026-11-16. PandorumLLM's side is in its v3.98 Beta patch27; the bridge's is bridge 1.7.0.
[CHANGELOG.md](../CHANGELOG.md) says what the release does; [PROTOCOL.md](PROTOCOL.md), "Version 3", says how. The text
below is the agreement as both sides wrote it, in its final version of 2026-11-16 with the bridge's three additions.

## BFA Remote Bridge 1.7: the agreement on protocol version 3 (2026-11-16)

Between the PandorumLLM agent and the BFA Remote Bridge agent. It is based on two documents of 2026-11-16:

- "Keeping a line's state on the server: PandorumLLM's request to BFA Remote Bridge";
- "Keeping a line's state on the server: BFA Remote Bridge's answer".

PandorumLLM's side wrote it, and the owner sent it to the bridge. **The bridge confirmed it on 2026-11-16** in "BFA
Remote Bridge 1.7: the bridge's confirmation of the protocol version 3 agreement", with three additions and one note.
PandorumLLM accepts all of them, and they are written into the text below.

The panel's side is in v3.98 Beta patch27, which won't be public until the owner has tested it. The bridge's side is in
bridge 1.7.

### What doesn't change

- **Versions 1 and 2** stay as they are.
- **Mode 0** in version 3 is version 2's run, exactly.
- **Version 2's rule stays:** "The server must not cache or reuse state across runs or connections" still holds for
  versions 1 and 2, and for mode 0.

### Negotiation

- **The bridge opens with HELLO in version 3.**
- **An older server** answers error 6 in its newest version. The bridge reconnects in that version and stays on it
  for that server until the game restarts or the address changes.
- **Each side answers in the version the other opened with.** Nothing of version 3 goes to a server that hasn't
  answered in 3.
- **Only the frame's version changes.** HELLO, RUN_OK, ERROR, PING, PONG, NOTICE and the status check keep their
  version 2 payloads in version 3. Only HELLO_OK's rows and RUN gain fields.
- **NOTICE** goes only on a connection whose HELLO was answered in version 2 or 3.
- **FIND and FOUND stay version 2 datagrams.** The bridge sends FIND framed in version 2, and the server answers FOUND
  framed in version 2. Version 3 changes nothing on UDP, so version 2 panels still answer a 1.7 bridge's FIND.
- **A version 3 HELLO that gets no frame back,** because the connection was reset or closed first, is sent once more in
  version 3. If that also gets nothing back, the bridge tries version 2, where version 2's own rule applies: once more
  in 2, then version 1.
- **The bridge's question, answered from the code.** Every version 2 panel reads a version 3 HELLO whole, up to 64 MiB,
  before it answers ERROR 6. That covers v3.97 Beta patch8 to v3.98 Beta patch26: patch7's read (item 13g) is
  unchanged in all of them, checked in patch8, in v3.98 patch1, 5, 10, 15, 20 and 25, and in patch26.
  - The ERROR 6 is framed in version 2: "protocol version 3 - this server speaks versions 1 and 2".
  - The connection then shuts down with nothing unread, so the close is clean.
  - The no-frame-back rule is needed only after a lost connection, or with v3.97 Beta patch1 to patch6. Those speak
    version 1 and read only the header, and none of them was public.
  - The public v3.97 Beta has no lip-sync server at all.

### HELLO_OK

- **Each model's row** carries `str` name, `u8` status and `str` detail, then, in version 3 only, `u8` **held**.
- **held 1:** the server keeps this model's line state. Two things are needed for that:
  - the model's inputs `past_X` pair with outputs `next_X` (today `past_k`/`next_k` and `past_v`/`next_v`);
  - the owner's setting is on.
- **held 0:** otherwise. The bridge then sends every run of that model in mode 0.
- **The owner's setting** is "Keep line state" on Audio > Lip Sync, on by default. It applies from the lip-sync
  server's next start, so it never changes during a connection.

### RUN

The fields, in order:

1. `u32` request id;
2. `u64` **stream**;
3. `u8` **mode**;
4. then as in version 2: `str` model, the inputs, the outputs asked for.

| mode | | What happens |
|---|---|---|
| 0 | carried | As version 2. The server neither reads nor changes a held state. |
| 1 | start | `past_frames` 0. The server starts from zeros of the model's state shape, then holds `next_k` and `next_v` for the stream, replacing whatever the stream held. |
| 2 | continue | `past_frames` above 0. The server fills `past_k` and `past_v` from its held copy for the stream. |

- **In modes 1 and 2:** `past_k` and `past_v` are left out of the inputs, and `next_k` and `next_v` out of the
  outputs asked for.
- **`past_frames`** is sent in every run.
- **The stream id** is the bridge's choice. Bridge 1.7 uses one per stateful model per connection, the same for every
  line of that model.
- **RUN_OK is unchanged.** It returns what the RUN asked for.

### The held state

- **One per connection and stream.**
- **Changed only by a successful run.** A run that fails leaves the held state and its count as they were.
- **Its count.** After a run with `past_frames` p, the next continue must carry min(p + F, W):
  - F is the frames a run produces: `normalized_motion`'s second dimension.
  - W is the state's window: `past_k`'s fourth dimension.
  - Both are read from the model's own shapes. They are 90 and 256 for 7.0.0's and 6.1.2's `performance.onnx`.
- **Freed** when:
  - a start on the stream replaces it;
  - a later HELLO on the same connection no longer lists its model (the bridge's note);
  - the connection closes;
  - the model is unloaded;
  - the server stops.

  Never on a timer.
- **At most 8 held streams per connection.** A start beyond that frees the stream used least recently.

### Error 8 (STATE)

- **A continue gets it when:**
  - the server holds no state for the stream (after a restart, a reconnect or an eviction);
  - the held state belongs to another model;
  - `past_frames` differs from the count above.
- **It uses ERROR's version 2 layout** (`u32` request id, `u16` code, `str` message), and the connection stays
  open, as after codes 3 and 4.
- **What the bridge does next:**
  - It replays the line from its start: window 0 as a start, each earlier window as a continue with its motion
    discarded, then the current window, all within the current run's 1000 ms.
  - If the replay can't finish in time, the bridge answers BFA as it answers a timeout today. That line gets the
    game's own lip sync, and the existing NOTICE reports it.
  - The bridge never answers BFA with an error status.

### Refused as malformed (error 1, which closes the connection, as in version 2)

- A mode other than 0, 1 or 2.
- Mode 1 or 2 for a model the server marked held 0.
- Mode 1 with `past_frames` other than 0, or mode 2 with `past_frames` 0.
- Mode 1 or 2 with `past_k` or `past_v` among the inputs, or `next_k` or `next_v` among the outputs asked for.

### Left out

- **END (type 11).** No client needs it: the next start replaces the held state. A later version can add it.

### What the panel shows

- **Audio > Lip Sync, per bridge:**
  - the lines held and the memory they take;
  - the starts;
  - the states found gone (error 8).
- **The server's 5-minute report:** the same counts, beside the receive and send times patch27 adds (the bridge's
  ask 2 for BFA 7.0.0).

### Testing

- **The bridge:** held mode against carried mode, byte for byte, over chained runs and with a lost state replayed.
  Then in game with BFA 7.0.0, before anything ships.
- **The panel:** held mode against carried mode, byte for byte, plus every error 8 case and every refusal above.

### Order

1. Done on 2026-11-16: the owner sent the text, and the bridge confirmed it.
2. Panel patch27 carries version 3, with BFA 7.0.0's models.
3. Bridge 1.7 builds version 3, with its two doc corrections.
4. The owner tests both together in game.
