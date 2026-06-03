#include <tcl.h>
#include <string.h>
#include <emscripten.h>
#include "surftcl.h"

/*
 * Main-thread notifier for SurfTcl.
 *
 * On the web the JS event loop owns the bottom of the stack; Tcl is a
 * guest of it, not the other way round. So Tcl must never *block* waiting
 * for an event — there is no thread to wake it, and a blocking wait would
 * freeze the page. This notifier therefore refuses to block: its
 * waitForEvent returns immediately. The JS side drives event servicing
 * explicitly by calling SurfTcl_ServiceEvents (which pumps Tcl_DoOneEvent in
 * non-blocking mode) whenever it gets a turn — a channel got bytes, a
 * timer is due, a DOM event fired.
 *
 * What this buys, beyond not freezing: `vwait`/blocking `Tcl_DoOneEvent`
 * fail *fast* and *honestly*. With nothing to block on, DoOneEvent returns
 * 0 after a single empty pass and vwait raises `TCL EVENT NO_SOURCES`
 * ("would wait forever") instead of hanging the tab. That is the correct
 * answer on the main thread: you cannot vwait on an event that only the
 * JS loop (which this very call is blocking) could ever produce. Bind a
 * fileevent / after callback instead.
 *
 * The notifier is deliberately almost all stubs. Reflected channels post
 * their readability onto Tcl's generic event queue (Tcl_QueueEvent via
 * `chan postevent` / Tcl_NotifyChannel), and `after` timers live on Tcl's
 * own timer queue — both of which Tcl_DoOneEvent drains on its own,
 * regardless of the platform notifier. So the file-handler and timer
 * hooks have nothing to do here; the channels carry the events and the
 * pump dispatches them. The notifier's only real job is to stop trying to
 * block.
 */

// FIXME(dther) The above assumptions were based on the idea that all yields
// must be explicit and co-operative. The thing is, it's reasonable to mentally
// model the JS side as an UI event source, because... That's literally what it is.
// `vwait` might very well be waiting on a variable or a JS-controlled channel
// driven by an event callback.

// We still don't want to ever truly block the browser.
// But a *JS yield is a blocking call* from our point of view,
// and so it's probably right to refactor SurfTclWaitForEvent, at least,
// such that it yields to the JS side.

static void *SurfTclInitNotifier(void)            { return NULL; }
static void  SurfTclFinalizeNotifier(void *cd)    { (void) cd; }
static void  SurfTclAlertNotifier(void *cd)       { (void) cd; }
static void  SurfTclServiceModeHook(int mode)     { (void) mode; }

/* Tcl keeps its own timer queue; the notifier timer hint is irrelevant
 * because we never sleep on it. */
static void  SurfTclSetTimer(const Tcl_Time *t)   { (void) t; }

/*
 * Never block — but the *way* we decline matters. When Tcl wants to block
 * indefinitely (timePtr == NULL: "wait until something happens"), we must
 * return -1, the notifier's "can't wait, would deadlock" signal — the same
 * value the stock select notifier returns when it has no fds and no timeout
 * (tclSelectNotfy.c). That makes a blocking Tcl_DoOneEvent give up at once,
 * so vwait raises NO_SOURCES instead of spinning forever. (Returning 0 here
 * instead means "timed out, nothing happened," which a blocking DoOneEvent
 * reads as "try again" — an infinite busy-loop that hangs the page.)
 *
 * For a finite timeout (a poll, e.g. Tcl_DoOneEvent under TCL_DONT_WAIT
 * hands us a zero Tcl_Time), 0 is correct: we polled, nothing of ours was
 * ready, fall through.
 */
static int
SurfTclWaitForEvent(const Tcl_Time *timePtr)
{
    if (timePtr == NULL) {
        return -1;
    }
    return 0;
}

/* Reflected channels don't register real fds with the notifier, so these
 * never fire for our channels; stubbed for completeness. */
static void  SurfTclCreateFileHandler(int fd, int mask, Tcl_FileProc *proc,
                                   void *cd)
{ (void) fd; (void) mask; (void) proc; (void) cd; }
static void  SurfTclDeleteFileHandler(int fd) { (void) fd; }

void
SurfTcl_InstallNotifier(void)
{
    Tcl_NotifierProcs np;
    memset(&np, 0, sizeof(np));
    np.initNotifierProc      = SurfTclInitNotifier;
    np.finalizeNotifierProc  = SurfTclFinalizeNotifier;
    np.alertNotifierProc     = SurfTclAlertNotifier;
    np.serviceModeHookProc   = SurfTclServiceModeHook;
    np.setTimerProc          = SurfTclSetTimer;
    np.waitForEventProc      = SurfTclWaitForEvent;
    np.createFileHandlerProc = SurfTclCreateFileHandler;
    np.deleteFileHandlerProc = SurfTclDeleteFileHandler;
    Tcl_SetNotifier(&np);
}

/*
 * Drain every event that is ready right now and return how many were
 * serviced. This is the pump the JS side calls each loop turn. TCL_DONT_WAIT
 * guarantees we never block; the loop keeps going until nothing more is
 * ready, so a single call fully flushes whatever the page just handed us
 * (channel bytes, due timers, idle tasks) before returning control to JS.
 */
int
SurfTcl_ServiceEvents(void)
{
    int serviced = 0;
    while (Tcl_DoOneEvent(TCL_ALL_EVENTS | TCL_DONT_WAIT)) {
        serviced++;
    }
    return serviced;
}

/*
 * SurfTcl_Yield — the Tcl-side `await`. Relinquish the wasm thread to the JS
 * event loop and resume *in place*. emscripten_sleep(0) unwinds the wasm
 * stack (Asyncify) back to the JS loop, lets it drain one turn — pending
 * DOM events, timers, repaint — then a setTimeout(0) rewinds and resumes
 * right here. This is what `update` (wrapped in Tcl) calls to let the
 * substrate breathe; there is no pure-Tcl way to do it.
 *
 * The one-suspension guard. While we're suspended, JS may re-enter Tcl,
 * and a re-entrant evaluation that *itself* yields would put a second
 * unwind in flight — two suspensions interleaved, whose resume order can
 * violate the strict LIFO discipline that both Tcl's segmented execution
 * stack (tclExecute.c GrowEvaluationStack) and classic Asyncify's single
 * unwind buffer assume. A re-entrant eval that runs to *completion* is
 * fine and stays allowed (it's just a temporally-stretched nested call);
 * only a nested *yield* is refused, before it can reach emscripten_sleep.
 *
 * Requires an Asyncify (or, later, JSPI) build. The default build leaves
 * SURFTCL_ASYNCIFY undefined, so the command is present but reports that it
 * needs a yield-capable runtime rather than failing to link.
 */
static int surftclYieldInFlight = 0;

int
SurfTcl_Yield(Tcl_Interp *interp)
{
    // FIXME(dther) SurfTcl_Yield should not allow background errors to unwind
    // the foreground.

    // Tcl has an idiomatic mechanism for this, `bgerror`, which correctly raises
    // a message on an appropriate channel (stderr in a REPL, an alert pop-up in Tk)
    // and allows the foreground computation to continue as normal.

    // I'm hesitant to mess with this because I don't know how to test this,
    // is the thing. In any case, the planned architecture will be shaped like this.

    // 1. Before the yield begins, stash the current stack, *somehow.*
    // 2. JS evaluations happen at the global level. This is already the case.
    // 3. If an error occurs, emit an error, as well as the stack, to `bgerror`.
    // 4. Consider accumulating information like "time spent in JS side" and
    //    "number of event callbacks processed" and "evaluations which occurred."
    // 5. When the yield completes, *regardless of what happens,*
    //    the stack at point-of-yield is restored and the result is set
    //    with return code TCL_OK.
    //    SurfTcl_Yield will return successfully, with the result reflecting
    //    diagnostic info which the yield caller may opt to ignore.

    // In this way, the primary computation's call stack remains unpolluted,
    // allowing it to be debugged independently of JS callbacks,
    // but background errors are correctly logged and presented as they occurred using
    // a Tcl-native mechanism that a developer can override in the way they
    // see fit through `interp bgerror`.

    // The default bgerror will be decided later.
    // It will likely be emitting a user-visible alert pop-up, or a Web Component,
    // if DOM access is granted.

    // It really feels like there should be a Tcl C API way to do this.

    // FIXME(dther) There needs to be more than one kind of yield.
    // Basically, we want to be able to hint to the JS side that a yield is for
    // a specific reason. Those hints being, at minimum:

    // - Whether or not `Eval` is allowed to be called
    // - Whether or not JS computation is allowed to take a
    //   subjectively long amount of time
    //   - I say "subjectively" because the end goal is multimedia applications,
    //     and the context of "long" varies. A second longer processing a big
    //     file is probably fine. A second longer drawing a frame
    //     means we have an FPS of 1
    //   - should the Tcl side optionally suggest a timeout? Yes, yes it should.
    //     0 means "ASAP" and -1 means "idk, whenever."
    // - Whether or not a redraw is requested (requestAnimationFrame)
    // - Whether or not the interp is "done" (doesn't expect to have any more events)

    // So, common situations...
    // - `update idletasks`: Eval is not allowed. (?)
    //   timeout is 0, because we expect to get back to executing immediately.
    //   If an earlier command would have changed the display, requestAnimationFrame.
    //   (this is not always the case, and it's the command's responsibility
    //   to flip the flag to "please request" via Tcl_DoWhenIdle or similar).
    //   Also, we aren't done, Tcl expects to run again.
    // - `vwait`: As `update idletasks` above, but Eval is allowed.
    // - Event queue is empty: As `vwait` above, but timeout is -1 by default,
    //   adjustable by the developer through an API to be devised later.
    //   - cases in point: Code editor waits until the user presses a button.
    //     Video game demands max priority physics updates 30-60 times per second.
    // - Event queue is empty, no event sources left: Tcl doesn't expect to run again.
    //   this is distinct from a timeout of -1, which might still *have* events,
    //   but is asking the JS side to intentionally deprioritise Tcl's execution.

    // The JS side can choose not to respect yield hints, but our JS tries its best
    // to do so.
    // "Hint was ignored" should return a `SURFTCL YIELD ...` exception describing
    // *which* hints were ignored, *but only to the caller of `surftcl::js::yield`.*
    // Most developer end-users of SurfTcl will never have to yield directly,
    // and only use it through `update` or `vwait`.
    // *Those should be a background error,* silenceable on a case-by-case basis,
    // and in the case of Evals, raised in JS at the point at which the
    // "no Evals" hint was violated and *not* the innocent command that asked.

    // e.g., "timeout" hints are easy to accidentally violate due to, say,
    // the user switching tabs and our tab being de-prioritised.
    // This should be catchable in an easy way, possibly not at all,
    // because if timeouts are being violated, the user's hardware is overstressed.

    // TODO(dther) ... And also, __Yields_Cannot_Nest.__ So no calling `vwait` inside
    // `update`, or vice versa, or vwait-on-vwait and update-on-update.
    // This is a hard limitation of asyncify, and it's considered bad Tcl practice
    // anyway. The recommendation is `after 0` and/or `coroutine`.

    // oh god this is like emmy's animation
    if (surftclYieldInFlight) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj(
            "surftcl: can't yield - an evaluation is already suspended at a "
            "yield point; defer with `after 0 [list ...]`", -1));
        Tcl_SetErrorCode(interp, "SURFTCL", "YIELD", "NESTED", (char *) NULL);
        return TCL_ERROR;
    }
#ifdef SURFTCL_ASYNCIFY
    surftclYieldInFlight = 1;
    emscripten_sleep(0);
    surftclYieldInFlight = 0;
    /* Our own result, not whatever a re-entrant eval left behind. */
    Tcl_ResetResult(interp);
    return TCL_OK;
#else
    Tcl_SetObjResult(interp, Tcl_NewStringObj(
        "surftcl: ::surftcl::js::yield requires an Asyncify build", -1));
    Tcl_SetErrorCode(interp, "SURFTCL", "YIELD", "UNSUPPORTED", (char *) NULL);
    return TCL_ERROR;
#endif
}
