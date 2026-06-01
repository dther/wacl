# Event-loop integration — design direction

Status: **working direction from the architecture discussion**, not yet
built. Records the decision, the reasoning, and the verified Tcl API the
implementation will rest on, so we resume cleanly rather than re-derive.

## The decision

Integrating the **JS event loop with the Tcl event loop on the main
thread** is the primary target. The worker-thread model is secondary —
kept for the clean-separation / reliable-CRUD case, not the default.

### Why main-thread-first (Tk is the floor, not the ceiling)

The worker model sequesters Tcl off the main thread so it can block
honestly (its own loop, `Atomics.wait` notifier). That's great for Tk
and CRUD, but a Web Worker has **no DOM and no access to the main-thread
graphics/audio/input surface**. The point of SurfTcl is publishing
desktop-first Tcl apps *and games* to the web without friction, and the
things games need —

  - **sound**
  - **hardware-accelerated graphics (WebGL)**
  - **non-keyboard/mouse input** (gamepad, a *stateful* keymap — Tk
    assumes input is text-entry-first and gives no stateful key state)

— are exactly the SDL3-through-Emscripten surface, all of which lives on
the **main thread**. A worker can't reach any of it. So the worker model
quietly assumes Tk is the ceiling; it's the floor. Main-thread
integration is what lets SurfTcl reach the whole platform.

This doesn't waste prior thinking: we still need async data passing,
event-queue semantics, and a JS-side rework. It just means we do **not**
pull in worker/SharedArrayBuffer/`Atomics` semantics yet.

## What the Tcl source actually says (verified against 9.0.3)

Reading `tcl/unix/tclSelectNotfy.c` + `generic/tclNotify.c` directly:

- **The notifier thread is woken by a self-pipe, not signals.** The
  `triggerPipe` (a real pipe fd) is what the notifier thread `select()`s
  on alongside registered fds; `Tcl_AlertNotifier` and friends wake it by
  `write()`ing that pipe (tclSelectNotfy.c lines ~755/837). The earlier
  CLAUDE.md claim that "Tcl 9's notifier uses `pthread_kill` to wake the
  notifier thread" was **wrong**.

- **`pthread_kill` is only on the async-signal path.**
  `TclAsyncNotifier` (line ~955) uses it to *re-route an OS signal* to
  the notifier thread for Tcl's async/trap machinery — nothing to do
  with normal event wakeup. The reason `TCL_THREADS=0` is still needed is
  a **link-time** one: with threads on, that signal path references
  `pthread_kill`, which Emscripten (built without `-pthread`) doesn't
  provide, so the link fails. Not a runtime wakeup dependency.

- **The notifier is fully overridable without patching Tcl.**
  `Tcl_SetNotifier(Tcl_NotifierProcs *)` swaps all eight procs:
  `setTimerProc`, `waitForEventProc`, `createFileHandlerProc`,
  `deleteFileHandlerProc`, `initNotifierProc`, `finalizeNotifierProc`,
  `alertNotifierProc`, `serviceModeHookProc`. The generic
  event-service layer above it — `Tcl_DoOneEvent(flags)`,
  `Tcl_ServiceAll()`, `Tcl_SetServiceMode()` — is platform-independent.
  Relevant flags: `TCL_DONT_WAIT` (service without blocking),
  `TCL_ALL_EVENTS`, and the per-source `TCL_{FILE,TIMER,IDLE}_EVENTS`.

That combination is the lever: a **custom non-blocking notifier** plus
**JS driving the pump** needs no Tcl-source patch (consistent with our
"we don't patch the Tcl tree" stance) and links nothing signal-related.

## The architecture

JS owns the bottom of the stack. Tcl is a cooperative guest of the JS
event loop:

1. Install a custom notifier via `Tcl_SetNotifier` whose
   `waitForEventProc` **never blocks** (returns immediately) — timing is
   JS's job, not Tcl's. `setTimerProc` maps to a JS `setTimeout`;
   file-handler registration records interest in our in-memory channels.
2. Service mode is `TCL_SERVICE_ALL`. When JS gets a turn (a timer fired,
   a DOM event arrived, channel bytes landed), it calls a new C entry
   point that runs `Tcl_ServiceAll()` / `Tcl_DoOneEvent(TCL_DONT_WAIT)`
   in a bounded drain, dispatching Tcl's ready fileevents/after-callbacks,
   then returns to JS. Tcl's loop "flushes whenever it isn't empty,"
   exactly as floated.
3. We trust JS not to starve us — to keep pumping and to deliver the DOM
   events we subscribed to. That's the polite-guest contract: the web is
   a host we're a guest in, and a polite guest cooperates with the host's
   loop instead of trying to seize the stack.

### Channels as the universal carrier

Rather than wiring JS into notifier internals, wire **just the channels**
and let the bytestream be the abstraction everything else rides on:

- A real-time FIFO is an in-memory byte buffer exposed as a Tcl channel
  (reflected channel / custom channel type — the `wacl::chan` machinery
  is already this shape). JS appends bytes and marks the channel readable
  (`Tcl_NotifyChannel` / a queued event); the next pump fires the
  channel's `fileevent readable`.
- **Real-time stdio** is the first instance: stdin/stdout/stderr become
  these channels, so `gets stdin` and friends participate in the event
  loop instead of the current JS-driven one-shot `pushStdin`.
- Generalize to **runtime-created FIFOs**: `surftcl::fifo NAME` (or
  similar) mints a fresh channel JS can attach to. DOM events, fetch
  responses, WebSocket frames, gamepad polls — all dispatch into Tcl as
  bytes on a channel, and Tcl can `fileevent`, buffer, reflect, and
  reconfigure them *because they're just strings*. A bytestream into
  local memory costs essentially nothing — it's a string buffer — yet it
  gives Tcl full control over how events are framed and handled.

### The constraint that does NOT go away

On the main thread you **cannot** `vwait` on a JS-sourced event. `vwait`
loops on `Tcl_DoOneEvent` until its variable is set; on the main thread
that loop can't yield to JS, so a JS-produced event can never arrive to
satisfy it (`would wait forever`, `TCL EVENT NO_SOURCES` — exactly the
red `chan-fileevent-1.1` marker). The main-thread idiom is therefore
**callback / fileevent-driven**, mirroring JS itself. `vwait` stays valid
only for waits a single pump can satisfy from Tcl's own queue. The honest
framing: on the main thread, Tcl adopts JS's "register interest, return"
discipline; blocking `vwait` is a worker-mode privilege.

### Eval-fence implication

JS-driven pumping is **not** re-entrant `Wacl_Eval` — the pump calls
`Tcl_ServiceAll`/`Tcl_DoOneEvent`, which dispatch callbacks via Tcl's own
`Tcl_EvalObjEx`, not through our `Wacl_Eval` entry. So the fence won't
trip on pumps. It should be narrowed to refuse only genuinely *nested
synchronous* `Wacl_Eval` (JS Eval → puts → output sink → JS Eval), while
letting loop-driven pumping and the fileevent callbacks it dispatches
run freely. Once pumping exists, the fence's own `after 0` / `after idle`
advice becomes actually true instead of a dead end.

## Deferred (worker mode + shared state)

- **Worker mode**, when clean separation is the goal: Tcl on a Web Worker
  with an `Atomics.wait`/`Atomics.notify` notifier so it can block
  honestly without freezing the UI. Needs `SharedArrayBuffer`, hence
  cross-origin isolation (`COOP: same-origin` + `COEP: require-corp`).
  Worker has no DOM, so every DOM/graphics op is a main-thread round-trip
  — fine for CRUD/Tk, fatal for the games surface, which is why it's
  secondary. Worker is chosen at boot, not migrated to (a live wasm
  instance + memory + JS closures can't cross the boundary).
- **`tsv` analogue** (Tcl Thread shared values — a lock-protected shared
  hash, no relation to the file format): maps onto `SharedArrayBuffer` +
  `Atomics`-based locking. Worker-mode concern; later.

## Progress

**Step 1 done (spike, `opt/waclNotifier.c`).** Custom non-blocking
notifier installed via `Tcl_SetNotifier` at the top of `main`, plus an
exported `Wacl_ServiceEvents` that drains ready events with
`Tcl_DoOneEvent(TCL_ALL_EVENTS | TCL_DONT_WAIT)`. Verified in the wasm
runtime: `after 0` + a `Wacl_ServiceEvents()` call fires the timer (pump
dispatches); a channel fed from JS fires its `fileevent` callback with no
`vwait`; and `vwait` on a JS-gated event now raises `NO_SOURCES`
*immediately* instead of hanging. No regression in the suite (45 pass).

**Lesson, the hard way:** `waitForEventProc` must return **-1** when
asked to block indefinitely (`timePtr == NULL`), not 0. Returning 0
("timed out, nothing happened") makes a blocking `Tcl_DoOneEvent` loop
and ask again — an infinite busy-spin that hangs the page. -1 is the
notifier's "can't wait, would deadlock" signal (the stock select notifier
returns it for the no-fds/no-timeout case); it's what makes `vwait` fail
fast. For a finite timeout (a poll), 0 is correct.

**Test acceptance, partly in.** `chan-fileevent-1.1` is rewritten to the
real model — JS feeds bytes, `chan postevent` signals readable, `update`
(the Tcl-side sibling of `Wacl_ServiceEvents`) pumps, the `fileevent`
fires and drains the bytes — and now **passes** in both runners, no
`vwait`. Headless CI is green (46 pass / 0 fail / 26 skip). The lone
remaining `eventLoop` marker is `dom-bind-3.1`: a DOM event has no
Tcl-side `postevent` to pump, and triggering it with `wacl::dom call …
click` fires the listener synchronously inside the current `Wacl_Eval`,
whose re-entrant `wacl.Eval` the fence refuses. It's now blocked
specifically on step 2 (the eval-fence), not the notifier — skips
headless, red in the browser.

## Next concrete steps

1. **Narrow the eval-fence** to genuinely nested-synchronous `Wacl_Eval`
   only, so a DOM listener firing from the JS loop (and the `after 0`
   advice the fence prints) actually works — which also flips
   `dom-bind-3.1` green.
2. **Real-time stdio as event-loop channels**, then runtime FIFOs. This
   is not just a C/bridge change: **the demos are all `interp.Eval(line)`
   -driven** (REPL, playground, tests page) and must be rewired to the
   pump/channel model — stdin becomes a channel JS feeds and
   `Wacl_ServiceEvents` drains, rather than a one-shot synchronous Eval
   per line. Touches `js/preJsRequire.js` and all three demo pages.
3. **Re-home `wacl::chan`'s JS-side deferral** onto `Wacl_ServiceEvents`
   instead of bare `setTimeout`, once the pump is the canonical loop.
