#include <tcl.h>
#include <string.h>
#include <emscripten.h>
#include "wacl.h"

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
