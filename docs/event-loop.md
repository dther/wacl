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
and let the bytestream be the abstraction everything else rides on. This
is for *application* channels — and they are **reflected channels, with
no Emscripten pipes**:

- An application event channel is an in-memory byte buffer exposed as a
  Tcl reflected channel (`chan create` — the `surftcl::chan` machinery is
  already this shape). JS appends bytes and marks it readable
  (`Tcl_NotifyChannel` / a queued event); the next pump fires the
  channel's `fileevent readable`. No Emscripten fd plumbing — the buffer
  is JS-side, fed across the bridge. With the re-entrancy fence gone and
  M1-style re-entry-during-a-suspension proven safe, JS can feed and post
  *synchronously* while the interpreter is yielded (the `setTimeout(0)`
  defer in `surftcl::chan` was only there for the old fence).
- Generalize to **runtime-created channels**: `surftcl::fifo NAME` (or
  similar) mints a fresh `surftcl::chan`-style channel JS can attach to. DOM
  events, fetch responses, WebSocket frames, gamepad polls — all dispatch
  into Tcl as bytes on a channel, and Tcl can `fileevent`, buffer,
  reflect, and reconfigure them *because they're just strings*. A
  bytestream into local memory costs essentially nothing.

**stdio is NOT one of these.** stdin/stdout/stderr are Tcl's own standard
channels — Tcl names and initialises them, over Emscripten `FS.init` fd
devices (the stdout-gotcha wiring). They stay in that standard-channel
layer; `surftcl::chan` is for app special-use channels, never the standard
streams. The "real-time stdin" payoff (a blocking `gets stdin` that
yields while it waits, instead of the one-shot `pushStdin`) belongs to
that device layer — same Asyncify yield, different home.

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

### Eval-fence: removed

The re-entrancy fence is **gone** (see Progress). It was solving the wrong
problem: re-entrant evaluation is normal and safe in Tcl, and the genuine
risk — corrupting a *suspended* evaluation's state — only arises at a
*yield* (see the yield design conclusion), not at a nested call that runs
to completion. JS-driven pumping never tripped it anyway (the pump
dispatches via Tcl's own `Tcl_EvalObjEx`, not `SurfTcl_Eval`).

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
exported `SurfTcl_ServiceEvents` that drains ready events with
`Tcl_DoOneEvent(TCL_ALL_EVENTS | TCL_DONT_WAIT)`. Verified in the wasm
runtime: `after 0` + a `SurfTcl_ServiceEvents()` call fires the timer (pump
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

**Test acceptance, in.** `chan-fileevent-1.1` is rewritten to the real
model — JS feeds bytes, `chan postevent` signals readable, `update` (the
Tcl-side sibling of `SurfTcl_ServiceEvents`) pumps, the `fileevent` fires
and drains — and passes in both runners, no `vwait`.

**Eval-fence removed** (commit "Remove requirement for re-entrant
execution") and locked in by `tests/wacl-bridge.test`: a JS callback may
re-enter `SurfTcl_Eval` synchronously, a thrown JS exception is a catchable
Tcl error, and a nested-`Eval` failure propagates with its `errorInfo`
trace intact (no save/restore — silent isolation is the thing we reject).
With the fence gone, `dom-bind-3.1` now passes for **synchronous**
dispatch: `surftcl::dom call … click` fires the listener inline and the
bound script's re-entrant `surftcl.Eval` runs. The `eventLoop` constraint is
retired (nothing uses it). What a test still can't reach is a *real*
event-loop-deferred click — that waits on the yield construct below.
Headless green (52 pass / 0 fail / 26 skip — dom skips, no DOM); jsdom
green across the board (78/78).

## Yield to the event loop — design conclusion

`vwait`/`update` assume Tcl owns the loop; we share it, so the missing
primitive is "yield to the substrate and resume in place" — Tcl's own
idiom for that is `update idletasks` ("let deferred display work happen"),
which on the web *is* "let the JS loop catch up." The transparent,
resume-in-place version requires stack switching — **Asyncify** (or the
lighter, standards-track **JSPI**); `setjmp`/`longjmp` can't do it
(`longjmp` only unwinds up, never resumes back into the computation).

The interp-safety worry turned out smaller than first feared, verified
against `tclExecute.c`:

  - **A single suspension with re-entrant evals that complete is safe.**
    Tcl's execution stack is *segmented* (`GrowEvaluationStack` adds a new
    `ExecStack` and retains the old; a nested allocation never moves or
    frees the parked evaluation's operands), so a re-entrant `Eval` during
    a yield is just a temporally-stretched nested command — `numLevels`,
    the `CallFrame` chain, and the operand stack all balance and restore.
    Asyncify saves/restores the C stack; the heap-resident interp
    structures are built to nest.
  - **The one real hazard is overlapping suspensions** — a re-entrant eval
    that *itself* yields, so resume order can violate the stack's LIFO
    discipline. `tclExecute.c:1034` panics ("Stack after current is in
    use") on exactly that, and classic Asyncify keeps a single global
    unwind state, so both layers assume one suspension in flight, LIFO.
  - **So the guard is a single flag, not a wall:** refuse to yield while
    another evaluation is already suspended ("defer with `after`"). Plus
    two correctness touches: JS-entry `SurfTcl_Eval` should force
    `TCL_EVAL_GLOBAL` (so a re-entrant eval during a yield doesn't inherit
    the parked proc's locals), and the yield command must set its own
    result on resume.

Real remaining costs are the mundane ones: Asyncify's size/speed tax
(prefer JSPI; scope with `ASYNCIFY_ONLY`), and — the big ripple —
**`interp.Eval` becomes async** (Promise-returning), which is the same
work as rewiring the Eval-driven demos. Going async is the through-line.

### The integration layer is pure Tcl (dther's wrapper)

The `update`→yield wiring does **not** need a C-level `Tcl_DoWhenIdle`
idletask. It's six lines of Tcl:

    rename update ::surftcl::OLD::update
    proc update {args} {
        ::surftcl::OLD::update {*}$args
        ::surftcl::js::yield
    }

"`update` = let the substrate breathe" — process Tcl's own queue, then
yield to JS. The mapping is honest (`update idletasks` already means
"let deferred display work happen"; on the web that's the JS loop). What
this layer can't conjure is `::surftcl::js::yield` itself — there is no
pure-Tcl way to relinquish the wasm thread to JS and resume in place
(`vwait` blocks; a coroutine `yield` returns to its Tcl resumer; `after`
just schedules). That primitive is irreducibly C + Asyncify. So the
wrapper is a thin shell *on top of* the spike, not a way around it.

Three things the wrapper forces immediately: (1) it makes nested yields
trivially reachable — any handler that runs during a yield and calls
`update` yields again — so the one-suspension guard is load-bearing from
day one (it turns that into a catchable error); (2) ordering is a real
choice — `OLD update; yield` returns with JS-fed events queued but
undispatched, so you may want `yield` then a re-drain to a fixpoint; (3)
yielding from inside a Tcl proc means the whole Tcl eval stack beneath it
(TEBC, dispatch, frames) must be Asyncify-instrumented to unwind, so
`ASYNCIFY_ONLY` scoping is impractical and you eat the broad tax.

### Mechanism: Asyncify now, JSPI later (decided)

Both Asyncify and JSPI give suspend/resume; the spike's findings are
mechanism-independent. The choice is about reach, and as of June 2026:

  - **JSPI** ships by default only in Chromium (Chrome 137+). Firefox has
    it behind a flag; Safari has no implementation yet (objection dropped
    late 2025, in progress under Interop 2026). So a JSPI-only build is
    Chrome-or-bust — fatal for "a polite guest on the *whole* web," and
    the same wrong trade as worker-mode-first.
  - **Asyncify** is a build-time code transform that emits a plain wasm
    module running on every engine today, including the node harness our
    spike and CI use (JSPI in node needs a recent V8 + maybe a flag).

So: **Asyncify for the spike and the cross-browser baseline; JSPI as a
progressive enhancement** (eventually a dual build — JSPI where present
to shed the tax, Asyncify everywhere else), wired only once Safari ships
so no one is excluded. Migrating the mechanism later is localized to the
`js::yield` binding + build flag, not the architecture.

## Yield spike — PASSED

Built (`make asyncify`, `-DSURFTCL_ASYNCIFY -sASYNCIFY
-sASYNCIFY_STACK_SIZE=1048576`) and run in node against the three
milestones. `SurfTcl_Yield` (opt/waclNotifier.c) is `emscripten_sleep(0)`
behind the one-suspension guard; `::surftcl::js::yield` fronts it (wacl.c);
JS-entry `SurfTcl_Eval` now forces `TCL_EVAL_GLOBAL`. Harness: dther's
pure-Tcl `update` wrapper + a `longcalc` proc that yields mid-computation
(`set x 1; update; incr x 41`).

  - **M0 — yield + resume in place: PASS.** `longcalc` suspends to the JS
    loop at the `update` and resumes, returning 42. Transparent yield
    works; the `update` wrapper is a real integration layer.
  - **M1 — re-entry during suspension: PASS, and this is the one that
    mattered.** While `longcalc` is parked at its yield, JS synchronously
    re-enters (`SurfTcl_Eval` "set ::reentered 1") — the re-entry runs *and*
    the parked computation resumes intact (42, `::reentered` == 1).
    Asyncify did **not** assert on the re-entry, and the segmented exec
    stack kept the parked operands valid. The interp-corruption fear was
    unfounded for a single suspension, exactly as the source reading
    predicted.
  - **M2 — nested-yield guard: PASS.** A yield attempted *during* the
    suspension is refused with `SURFTCL YIELD NESTED` ("already suspended")
    before any second `emscripten_sleep`, and the first suspension still
    resumes. No `tclExecute.c:1034` panic, no buffer clobber.

All milestones drove `SurfTcl_Eval` via `ccall(..., {async:true})`,
confirming the async boundary. Cost measured: **4.15 MB vs 2.82 MB**
(~1.47x) — the Asyncify tax, as expected. The C + Makefile target are
committed (guarded behind `SURFTCL_ASYNCIFY`, so the default build is
unchanged and stays lean); the asyncify wasm is **not** committed
(gitignored scratch).

## The async boundary — DONE

Asyncify is now the **committed baseline**: `make minimal` builds the
yield-capable ~4MB wasm (`-DSURFTCL_ASYNCIFY -sASYNCIFY`), the separate
`asyncify` target is gone. The JS bridge keeps **two** entry points,
which turned out to be the right shape rather than one async `Eval`:

  - **`interp.Eval(script)` — synchronous, unchanged.** For
    re-entrant/internal calls that need the result *now* and won't yield:
    the JS bridge re-enters this way (`::surftcl::js::call eval {surftcl.Eval
    "..."}` must hand a string back to the C bridge synchronously), and a
    sync ccall can't survive an unwind. The bridge tests and package
    shims ride this path untouched.
  - **`interp.EvalAsync(script)` — Promise, new.** The top-level entry for
    scripts that may yield (`ccall {async:true}`). Under Asyncify a
    yielding script unwinds to the JS loop and the Promise resolves on
    resume; non-yielding resolves immediately; errors reject with the
    `errorInfo` trace.

The REPL and playground drive user input through `EvalAsync` and install
the pure-Tcl `update` wrapper at boot, so a long command that `update`s
keeps the page live. Test runners stay on sync `Eval` (the suite never
yields), so headless CI is unchanged (52/0/26). Validated against the
committed wasm: sync Eval, EvalAsync-yields-and-resumes, re-entry during
a suspension, and async error propagation all pass.

## Next concrete steps

1. **Blocking `gets stdin` via yield** — in the standard-channel /
   `FS.init` device layer (stdin is Tcl's own channel, *not* a
   `surftcl::chan`): an empty read yields, JS feeds via `pushStdin` during
   the suspension, the read resumes. Keeps the page live while waiting.
   (Application event channels are already covered by `surftcl::chan` — see
   "Channels as the universal carrier"; nothing more needed there.)
2. **Async DOM listeners.** `surftcl::dom`'s bound handler still calls the
   *synchronous* `surftcl.Eval` (so a handler that itself `update`s would
   break, and `:this` isn't preserved across a yield). Move it to
   `EvalAsync` with proper currentElement save/restore across the await
   when we want handlers to be able to yield.
3. **Re-home `surftcl::chan`'s JS-side deferral** onto `SurfTcl_ServiceEvents`
   instead of bare `setTimeout`, once the pump is the canonical loop.
4. **JSPI** when Safari ships it (Interop 2026): a dual build that sheds
   the Asyncify size/speed tax where the engine supports it. The C and
   the `update` wrapper are mechanism-agnostic, so it's a localized swap.

## Runaway loops — the cooperative-model weakness (pre-release must-fix)

A Tcl loop that never yields (`while 1 {puts lol}` — no `update`) freezes
the whole browser tab: the single thread is held, the JS loop can't turn.
Confirmed in-browser. This is the inherent cost of cooperative scheduling.

A JS watchdog can't catch it (the timer is frozen by the loop). But Tcl
preempts *itself*: `Tcl_LimitCheck` (tclInterp.c:3441) **polls** the wall
clock at command-granularity checkpoints during execution and raises a
catchable `TCL LIMIT TIME` error when an `interp limit … -time` deadline
passes — no event loop needed, so it works mid-freeze and the loop
unwinds, returning control to JS. The tab recovers; it's not "close the
tab," it's "your script was stopped." (Fires at Tcl command boundaries —
a pure-C loop wouldn't checkpoint.)

Two complementary fixes, both likely wanted:
- **Hard time limit** (`Tcl_LimitSetTime`) as the backstop for output-less
  loops, with the honest `supportURL` error. Tune below the browser's own
  ~10–15s unresponsive dialog vs. tolerance for legit long no-yield work.
- **Throttled implicit yield on stdout/stderr write** — interactive-shell
  unbuffered semantics: an output-producing loop yields as it writes
  (yield only if >X ms since the last, so it's cheap) and stays live.

## Punted wiring

- **Unhandled-exception callback.** Unhandled Tcl errors currently go to
  stderr — REPL-correct, app-wrong (the web's default is "log to console,"
  not "bug report, please"). Add an explicit JS-side sink for unhandled
  Tcl exceptions, alongside the existing `surftcl.onError`/`supportURL`
  surface. Wiring only; after the pipes work.

- **Conditional call-stack stash across a yield (dther's idea; no major
  objection).** We rejected blanket save/restore of interp state around a
  nested call because it wipes a propagated error's trace. The halfway
  refinement: stash errorInfo/errorCode *before* a yield, and **restore it
  only if the yield resumes cleanly.** Reasoning — if a JS-side exception
  happens *during* the suspension you want the full stack (don't restore);
  but if you simply came back, all that happened was "background tasks ran
  successfully," and the foreground command's stack shouldn't be polluted
  with the JS event-callback churn that ran between your call and some
  unrelated exception long after. This is sound, and it unifies with
  Tcl's existing **bgerror**: background event-callbacks that run during a
  yield are conceptually the same as `after`/`fileevent` callbacks, whose
  errors already route to the background-error handler rather than the
  foreground script. So the design is: route errors from re-entrant
  background evals during a yield to `bgerror`; restore the foreground
  errorInfo on clean resume. The mechanism for "an exception during the
  yield surfaces to the yielding command" (vs. silently going to bgerror)
  is the part to design. Tie-in with `interp bgerror` / `::tcl::Bgerror`.

  Why `bgerror` is the right home (dther): it is **not silent** — the
  REPL prints a `bgerror` line + stack to stderr; Tk pops an alert with
  the trace on *every* failure (twenty failing callbacks in a loop should
  be twenty alarms). That's the correct middle ground between swallowing
  an error and treating every exception as a total panic that unspools
  the interpreter. The cautionary tale is Ariane 5 Flight 501: an
  unhandled Ada `Operand_Error` (a 64→16-bit conversion overflow in the
  inertial unit) whose handling policy was "shut the unit down" — primary
  and backup ran the same code and died identically, vehicle lost. The
  fault wasn't the overflow, it was *exception ⇒ panic ⇒ tear everything
  down*. `bgerror`'s "be loud, but let the unrelated current evaluation
  proceed" is the safer default, and SurfTcl will support that workflow.
