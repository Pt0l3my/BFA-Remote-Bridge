# Bridge 1.1.0: the agreed scope

The bridge's side and PandorumLLM's side agreed this scope on 2026-10-18, from the owner's test logs of bridge 1.0.0
with PandorumLLM v3.97. **Protocol version 1 doesn't change.** [CHANGELOG.md](../CHANGELOG.md) says what 1.1.0 does;
[PROTOCOL.md](PROTOCOL.md) section 3 says how.

## The twelve items

1. **Connect timeout:** 3000 ms for connects off the speech path (the status check, a model load, the background
   thread). A connect inside a RUN stays within that RUN's time.
2. **Background thread:** after a failure, it reconnects and says HELLO with a backoff of 1, 2, 5, 10 and 30 s, then
   every 30 s. This replaces the flat 10 s pause: the pause ends when the background HELLO is answered. Runs during a
   pause are answered at once with NaN.
3. **Idle PING:** after 60 s idle, on the main connection after its HELLO. A PONG must come back within the RUN
   timeout; a missing one starts the background reconnect. The thread never holds the connection while a run waits.
4. **Reconnecting within a RUN** (already in 1.0.0): one reconnect, HELLO and resend for a connection that was closed,
   reset or failed to send. The log names which.
5. **One main connection:** a model load uses a short-lived side connection, then the full list goes on the main
   connection.
6. **Status check:** on a connection of its own (already in 1.0.0), and every 5 minutes when the counters changed.
7. **A single timeout keeps the connection.** The pause starts on a connection failure or a second timeout in a row.
8. **Each fallback logged:** its model, its reason (no connection, connect timed out, RUN timed out, ERROR with code
   and text, not ready, connection lost) and the time it took. Runs answered during a pause are counted by reason.
9. **The summary line** counts every run, by reason. It prints at each 250 runs and every 10 minutes when anything
   happened.
10. **The Detailed level adds** connects and closes with their reasons, each HELLO's answer, each unanswered PING and
    each background attempt.
11. **The GPU notice,** once per session, while the bridge is on and its notices are allowed: "BFA Remote Bridge: BFA
    is set to GPU. Its Force CPU setting keeps it off this PC's graphics card."
12. **The client string with counters,** "BFARemoteBridge 1.1.0 (answered N, fallbacks N, timeouts N)": within 80
    characters until the panel's log lines take 120, keys in that order and new keys only at the end, cumulative from
    Skyrim's start. "fallbacks" counts every run the server didn't answer; "timeouts" is a part of it.

## Added in the final answer

- **A late answer that ends the connection.** A discarded late answer that is an ERROR with code 1, 2, 5 or 6 is a
  connection failure: the server closes the connection after it. An ERROR with code 7 means the server is stopping:
  a failure too, and the background thread reconnects.
- **The background HELLO waits for the bridge's own load.** Until the panel's next patch, a HELLO on any connection
  waits while any load is in progress, so the background thread holds its HELLOs (a reconnect's, or the one after a
  missing PONG) while the bridge's side load is in flight. A status check names no model and never waits, so it isn't
  held. A RUN on another connection runs during a load (measured on the CPU runtime).
- **The one caveat:** DirectML's behaviour during a first load isn't measured yet. The load may slow the runs of the
  models already loaded on the same card, though nothing on the server makes them wait.

## Later, and dropped

- **Later:** a REPORT message (protocol 1.1), if the client string proves not enough.
- **Dropped:** request ids numbered by line. The bridge sees BFA's runs, not its lines.

## What the bridge chose

- The late answers: the connection is kept only when no byte of the timed-out answer had arrived. A timeout in the
  middle of a frame, or of the send, closes the connection and starts the pause.
- The 80 characters: when the counters would make the client string longer, the trailing keys are dropped, last
  first, until it fits; the name and version alone when none fits.
- A model the main connection's HELLO doesn't have ready: its runs get the NaN answer at once (the first logged, the
  rest counted), without a pause, and the background thread asks again on the backoff schedule.
- No new settings. The connect timeout, the backoff, the PING interval and the status and summary cadences are fixed.
