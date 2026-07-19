# Event-loop integration

Status: **the browser owns and pumps the loop.** Tcl runs as a
cooperative guest of the JS event loop: a custom notifier that never
blocks, plus `emscripten_set_main_loop` driving
`Tcl_DoOneEvent(TCL_DONT_WAIT)`. That is the whole current mechanism.

The Asyncify tier that used to be documented here — the
`::surftcl::js::yield` primitive, `interp.EvalAsync`, the JS-driven
`SurfTcl_ServiceEvents` pump, the pure-Tcl `update` wrapper — was built,
proven against its milestones, and then **removed**. The retrospective
below records why, and which findings carry over to the JSPI revisit,
so we resume cleanly rather than re-derive.

## Current mechanics (what's in the tree)

Two files:

- **`opt/surftclNotifier.c`** — installs, via `Tcl_SetNotifier` at the
  top of `main` (no Tcl-source patch), a notifier that refuses to
  block. Almost all of it is stubs, deliberately: reflected channels
  post their readability onto Tcl's generic event queue (`chan
  postevent` → `Tcl_QueueEvent`) and `after` timers live on Tcl's own
  timer queue — both drained by `Tcl_DoOneEvent` regardless of the
  platform notifier. The notifier's only real job is to stop trying to
  wait.
- **`opt/surftclAppInit.c`** — `main` installs the notifier, mounts the
  library zip, runs `Tcl_Init` + `SurfTcl_Init`, then registers
  `SurfTclMainLoop` with `emscripten_set_main_loop(loop, 0, 0)`: the
  browser calls it on every requestAnimationFrame tick, and it runs one
  `Tcl_DoOneEvent(TCL_DONT_WAIT|TCL_ALL_EVENTS)`. (TODO recorded in the
  code: flush the whole ready queue per tick instead of one event — if
  Tcl events ever visibly lag the browser, the one-event-per-frame
  cadence is the first suspect.)

What this gives, concretely:

- `after 1000 {puts hi}` fires on time with no page-side pump call —
  the rAF loop services it.
- A `fileevent readable` on a reflected channel fires once the
  readable event is posted and the queue is pumped (by the rAF loop, or
  synchronously by Tcl's own `update`).
- JS↔Tcl re-entrancy is synchronous and unrestricted (see the no-fence
  section in CLAUDE.md); the pump dispatches handlers via Tcl's own
  machinery.
- **Blocking waits fail fast and honestly.** `vwait` on a JS-gated
  event raises `TCL EVENT NO_SOURCES` ("would wait forever")
  immediately instead of hanging the tab. That is the correct answer on
  the main thread: you cannot vwait on an event that only the JS loop —
  which this very call would be blocking — could ever produce.

### The lesson baked into the notifier (learned the hard way)

`waitForEventProc` must return **-1** when asked to block indefinitely
(`timePtr == NULL`), not 0. Returning 0 means "timed out, nothing
happened," which a blocking `Tcl_DoOneEvent` reads as "try again" — an
infinite busy-spin that hangs the page. -1 is the notifier's "can't
wait, would deadlock" signal (the stock select notifier returns it for
the no-fds/no-timeout case, tclSelectNotfy.c); it's what makes `vwait`
fail fast. For a finite timeout (a poll, e.g. `TCL_DONT_WAIT` handing
down a zero `Tcl_Time`), 0 is correct: we polled, nothing was ready.

### The idiom

On the main thread, Tcl adopts JS's "register interest, return"
discipline: **callback / fileevent-driven** code, mirroring JS itself.
`vwait` stays valid only for waits a single synchronous drain can
satisfy from Tcl's own queue.

For blocking-*feel* control flow, the intended idiom is **coroutines
scheduled as events** — a `coroutine` that parks itself with
`after 0 [info coroutine]`-style continuations gives sequential-looking
code without suspending the wasm stack, using nothing but the Tcl event
loop. At some point this gets wrapped in something Promise-shaped on the
JS side ("run this, resolve with the result"), but not before real
developers have tried SurfTcl — the API doesn't get locked in early.

## What the Tcl source actually says (verified against 9.0.3)

Reading `tcl/unix/tclSelectNotfy.c` + `generic/tclNotify.c` directly:

- **The notifier thread is woken by a self-pipe, not signals.** The
  `triggerPipe` (a real pipe fd) is what the notifier thread `select()`s
  on alongside registered fds; `Tcl_AlertNotifier` and friends wake it
  by `write()`ing that pipe. (An earlier claim here that "Tcl 9's
  notifier uses `pthread_kill` to wake the notifier thread" was wrong.)
- **`pthread_kill` is only on the async-signal path.**
  `TclAsyncNotifier` uses it to *re-route an OS signal* to the notifier
  thread for Tcl's async/trap machinery. `TCL_THREADS=0` is needed for
  a **link-time** reason: with threads on, that signal path references
  `pthread_kill`, which our non-`-pthread` Emscripten build doesn't
  provide, so the link fails. Not a runtime wakeup dependency. A
  self-pipe-fallback TIP is still worth filing upstream for "threads
  but no signals" targets.
- **The notifier is fully overridable without patching Tcl.**
  `Tcl_SetNotifier(Tcl_NotifierProcs *)` swaps all eight procs. The
  generic event-service layer above it — `Tcl_DoOneEvent(flags)`,
  `Tcl_ServiceAll()`, `Tcl_SetServiceMode()` — is platform-independent.
  Relevant flags: `TCL_DONT_WAIT`, `TCL_ALL_EVENTS`, and the per-source
  `TCL_{FILE,TIMER,IDLE}_EVENTS`.

### Channels as the universal carrier

Wire **just the channels** and let the bytestream be the abstraction
everything else rides on. Application event channels are in-memory byte
buffers exposed as Tcl reflected channels (`chan create` — the
`surftcl::chan` shape), **no Emscripten pipes**: JS appends bytes and
posts readable; the next pump fires the `fileevent`. DOM events, fetch
responses, WebSocket frames, gamepad polls — all can dispatch into Tcl
as bytes on a channel, and Tcl can `fileevent`, buffer, and reconfigure
them because they're just strings.

**stdio is NOT one of these.** stdin/stdout/stderr are Tcl's own
standard channels over Emscripten `FS.init` fd devices (see the Stdio
section of CLAUDE.md). They stay in that standard-channel layer;
`surftcl::chan` is for app special-use channels, never the standard
streams. Known wart recorded as `DEFER(channel rework)`: the Emscripten
devices are never-blocking and can't signal "no data yet" as distinct
from EOF — the fix belongs in that device layer (EAGAIN-honest
devices), not in reflected channels.

## The Asyncify era — built, proven, removed (retrospective)

What existed at the peak (all deleted from the tree now):

- `::surftcl::js::yield` → `SurfTcl_Yield` → `emscripten_sleep(0)` under
  `-sASYNCIFY`, behind a one-suspension guard (`SURFTCL YIELD NESTED`).
- `interp.EvalAsync(script)` — Promise-returning top-level entry
  (`ccall {async:true}`); sync `Eval` kept alongside for re-entrant
  paths.
- `SurfTcl_ServiceEvents` — the explicit JS-driven pump (superseded by
  `emscripten_set_main_loop`).
- The pure-Tcl `update` wrapper (`rename update; proc update {…}
  {OLD::update; js::yield}`) as the "let the substrate breathe" idiom.
- A dedicated node harness (`tests/run-async.mjs`) that drove all of it,
  including interp-limit preemption and `interp cancel -unwind` halts.

The spike milestones all passed — transparent yield+resume, re-entry
during a suspension (the segmented exec stack kept the parked operands
valid), and the nested-yield guard. Measured cost: 4.15 MB vs 2.82 MB
wasm (~1.47×).

**Why it was removed anyway** (dther, after two weeks wrangling notifier
and FIFO semantics, including segfaults in the rendering thread):

- **Asyncify's global state is touched on *every* ccall entry**,
  asynchronous or not. A JS→C call made while another stack is
  suspended can silently clobber the suspended stack's bookkeeping —
  meaning any JS call into the C side is potentially *retroactively*
  unsafe, in a way that can't be detected or fenced locally. The
  one-suspension guard protected against nested *yields*, but not
  against this.
- The yield semantics ("suspending an evaluation while JS runs and
  re-enters Tcl") were judged too complex and too difficult to explain
  for too small a benefit, against the current goal of a small
  interpreter with a simple API to put in front of experienced Tcl
  developers.
- The size/complexity tax bought little that `coroutine` + the Tcl
  event loop can't express.

The browser has no native "safely call C without affecting another
suspended call" primitive yet. **JSPI** is that primitive — per-call
suspender objects instead of one global unwind state — and it becomes
the reason to revisit, once it's baseline across browsers (est. another
year; Chromium ships it, Firefox is behind a flag, Safari in progress
under Interop 2026). The revisit happens **after** the first public
alpha; no API gets locked in before real developers have tried SurfTcl.

### Findings that carry over to the JSPI revisit

Verified against `tclExecute.c` during the spike; mechanism-independent:

- **A single suspension with re-entrant evals that complete is safe.**
  Tcl's execution stack is segmented (`GrowEvaluationStack` adds a new
  `ExecStack` and retains the old; a nested allocation never moves or
  frees the parked evaluation's operands), so a re-entrant `Eval`
  during a suspension is just a temporally-stretched nested command —
  `numLevels`, the `CallFrame` chain, and the operand stack all balance
  and restore.
- **The one real hazard is overlapping suspensions** — a re-entrant
  eval that *itself* suspends, so resume order can violate the stack's
  LIFO discipline. `tclExecute.c:1034` panics ("Stack after current is
  in use") on exactly that. The guard is a single flag, not a wall:
  refuse to suspend while another evaluation is already suspended
  ("defer with `after`").
- **JS-entry `SurfTcl_Eval` forces `TCL_EVAL_GLOBAL`** so a re-entrant
  eval during a suspension doesn't inherit the parked proc's locals.
  This survives in `opt/surftcl.c` today — it's correct regardless
  (a JS-initiated evaluation is a fresh top-level call), and it's
  already in place for the JSPI era.
- The `update`-wrapper integration layer is pure Tcl (six lines); what
  it can't conjure is the suspension primitive itself — that is
  irreducibly C + stack-switching. Nested yields become trivially
  reachable the moment the wrapper exists, so the one-suspension guard
  is load-bearing from day one.
- A suspension-capable `interp.Eval` is necessarily async
  (Promise-returning); rewiring the Eval-driven pages is the same work
  as going async. Keeping a sync `Eval` alongside remained necessary —
  re-entrant bridge calls must hand a string back synchronously.

## Runaway loops — the cooperative-model weakness (pre-release must-fix)

A Tcl loop that never returns to the browser (`while 1 {puts lol}`)
freezes the whole tab: the single thread is held, the JS loop can't
turn. Confirmed in-browser. This is the inherent cost of cooperative
scheduling.

A JS watchdog can't catch it (the timer is frozen by the loop it would
watch). But Tcl preempts *itself*: `Tcl_LimitCheck` (tclInterp.c:3441)
**polls** the wall clock at command-granularity checkpoints during
execution and raises a catchable `TCL LIMIT TIME` error when an `interp
limit … -time` deadline passes — no event loop needed, so it works
mid-freeze; the loop unwinds and control returns to JS. The tab
recovers: "your script was stopped," not "close the tab." (Fires at Tcl
command boundaries — a pure-C loop wouldn't checkpoint.)

Of the two complementary fixes previously planned, only one survives
the Asyncify removal for now:

- **Hard time limit** (`Tcl_LimitSetTime`) as the backstop — still
  fully viable, needs no suspension primitive. Tune below the browser's
  own ~10–15s unresponsive dialog vs. tolerance for legit long
  computations; pair with the honest `supportURL` error surface.
- **Throttled implicit yield on stdout/stderr write** — needed the
  yield primitive; off the table until the JSPI tier.

(The Asyncify-era spike also proved the fancier variants — a limit
handler that yields and re-arms to keep a long command alive, and
`interp cancel -unwind` as an uncatchable halt pseudo-signal. Those
patterns are pinned in `tests/run-async.mjs`, which no longer runs —
keep it in git history for when suspension returns.)

## Deferred (worker mode + shared state)

- **Worker mode**, when clean separation is the goal: Tcl on a Web
  Worker with an `Atomics.wait`/`Atomics.notify` notifier so it can
  block honestly without freezing the UI. Needs `SharedArrayBuffer`,
  hence cross-origin isolation (COOP/COEP). A worker has no DOM and no
  main-thread graphics/audio/input surface — fine for CRUD/Tk, fatal
  for the games surface (sound, WebGL, gamepad — the
  SDL3-through-Emscripten stack lives on the main thread), which is why
  main-thread integration is primary: Tk is the floor, not the ceiling.
  Worker is chosen at boot, not migrated to.
- **`tsv` analogue** (Tcl Thread shared values): maps onto
  `SharedArrayBuffer` + `Atomics`-based locking. Worker-mode concern;
  later.

## Design notes kept for the async revisit

- **Unhandled-exception callback.** Unhandled Tcl errors currently go
  to stderr — REPL-correct, app-wrong. Add an explicit JS-side sink for
  unhandled Tcl exceptions alongside `surftcl.onError`/`supportURL`.
  Wiring only.

- **Conditional call-stack stash across a suspension (dther's idea).**
  Blanket save/restore of interp state around a nested call was
  rejected because it wipes a propagated error's trace. The refinement:
  stash errorInfo/errorCode *before* a suspension, and restore it
  **only if the suspension resumes cleanly** — if a JS-side exception
  happens *during* it you want the full stack; if you simply came back,
  the foreground command's stack shouldn't be polluted by the
  background churn that ran meanwhile. This unifies with Tcl's existing
  **bgerror**: background evals during a suspension are conceptually
  `after`/`fileevent` callbacks, whose errors already route to the
  background-error handler rather than the foreground script. `bgerror`
  is the right home because it is **not silent** — the REPL prints the
  trace, Tk alerts on every failure — the correct middle ground between
  swallowing an error and treating every exception as a panic that
  unspools the interpreter. (The cautionary tale is Ariane 5 Flight
  501: the fault wasn't the overflow, it was *exception ⇒ panic ⇒ tear
  everything down*.) The part still to design: the mechanism by which
  an exception during the suspension surfaces to the *suspending*
  command rather than silently going to bgerror.
