#include <tcl.h>
#include <string.h>
#include <emscripten.h>
#include "surftcl.h"

/*
 * Main-thread notifier for SurfTcl.
 *
 * TODO(dther) explain this once it works
 *
 */

/* Utility function for converting Tcl_Time structs to JS timeout milliseconds.
 * The maximum milliseconds a JS timeout accepts is the maximum positive int32,
 * 2,147,483,647, for historical reasons.
 */
#define MAX_INT32 (2147483647)
static int32_t
tclTimeMs(const Tcl_Time *timePtr)
{
    if (timePtr == NULL || timePtr->sec >= (MAX_INT32 / 1000))
        return MAX_INT32;
    long ms = (timePtr->sec * 1000) + (timePtr->usec / 1000);
    return (ms > MAX_INT32 ? MAX_INT32 : ms);
}

static void SurfTclFinalizeNotifier(void *cd)        { (void) cd; }
static void SurfTclServiceModeHook(int mode)         { (void) mode; }

/* SetTimer can be a stub when Tcl owns the event loop. */
static void SurfTclSetTimer(const Tcl_Time *timePtr) { (void) timePtr; }

static void *
SurfTclInitNotifier(void)
{
    EM_ASM({
        Module['__surftcl_timer'] = null;
        Module['__surftcl_wake'] = null;
    });
    return NULL;
}

EM_ASYNC_JS(bool, SurfTclWake, (void),
{
    if (Module['__surftcl_wake'] !== null) {
        Module['__surftcl_wake']();
        if (Module['__surftcl_timer'])
            clearTimeout(Module['__surftcl_timer']);
        return true;
    } else {
        return false;
    }
});

// TODO(dther) SurfTclAlertNotifier is basically a call of `Module.__surftcl_wake()`,
// *or* we set surftclAlerted, and the next WaitForEvent returns immediately.
static int surftclAlerted = 0;
static void
SurfTclAlertNotifier(void *cd)
{
    if (!SurfTclWake()) surftclAlerted = 1;
    /*
     * TODO(dther) consider this instead:
     * Instead of calling the resolve function (__surftcl_wake),
     * we just reconfigure its timeout to be 0.
     * This way, it executes as part of the next macrotask queue,
     * and means that all alerts get processed in one iteration,
     * rather than one full iteration-then-wait per alert.
     *
     * ... Actually, I think, for now at least, the current behaviour is good.
     * The discipline, therefore: AlertNotifier should be thought of as a "flush."
     * it's called when the browser thinks the Tcl side has time to execute a full
     * pass of the event loop, and should do so in the current microtask queue.
     * If multiple calls to AlertNotifier occur, we trust that the JS side
     * has good reason for doing so- e.g., a microtask enqueued a new event.
     * In this case, surftclAlerted = 1 at most asks for one more check before yielding.
     */
}

/* SurfTclWait yields to the browser *without* unwinding the stack.
 * It should only be used in SurfTclWaitForEvent.
 */
EM_ASYNC_JS(void, SurfTclWait, (int32_t timeout_ms),
{
    await new Promise((resolve) => {
        Module['__surftcl_wake'] = resolve;
        Module['__surftcl_timer'] = setTimeout(resolve, timeout_ms);
    });
    clearTimeout(Module.__surftcl_timer);
    Module['__surftcl_wake'] = null;
});
// TODO(dther) come up with better names for __surftcl_wake and __timer
// namespace to `Module['__surftcl']`?

/* SurfTclWaitForEvent yields to the browser using SurfTclWait in order to
 * process JS event callbacks.
 * It should return 1 after a yield, and -1 if it cannot do so.
 *
 * !!WARNING!! SurfTclWaitForEvent shouldn't return 0 after yielding,
 * as this will cause the entire browser to hang!
 *
 * WaitForEvent is called when Tcl_DoOneEvent finds nothing to do.
 * See Tcl_WaitForEvent for more information.
 */
extern int surftclEval; /* see SurfTcl_Eval in surftcl.c */
static int
SurfTclWaitForEvent(const Tcl_Time *timePtr)
{
    // A yield cannot occur because we're not running asynchronously
    if (surftclEval != 0) return -1;

    // surftclAlerted may be set by Tcl_AlertNotifier,
    // indicating that new events were queued that should be processed before waiting.
    if (surftclAlerted) {
        surftclAlerted = 0;
        return 1;
    }

    // Otherwise, always yield to the browser on a WaitForEvent if possible,
    // even when given a time of 0.
    SurfTclWait(tclTimeMs(timePtr));
    return 1;
}

// FIXME(dther) we're using actual stdio and executing the main loop now, so CreateFileHandler
// should probably work. Not much actually happens here. Basically:
// - when an fd is relevant, register it in a registry
// - WaitForEvent will scan this registry before yielding
// - OR, the JS side is guaranteed to flush what's been registered before passing back control
//
// This matters more in a Unix environment where things can be happening to FDs concurrently,
// but since the kernel doesn't really exist (Emscripten just simulates file events),
// it's equally correct to just have the JS side do `SurfTcl.AlertNotifier()` whenever
// it has a write that it wants the Tcl side to read.
//
// ... The thing is, I think whatever virtual channel mechanism Tcl is using to make this
// work doesn't care what we do here. It uses the standard syscalls to read, write and flush,
// and the Emscripten FS API handles it through the FS.init callbacks,
// which occur immediately, since, again, no kernel to actually worry about.
// Tcl-side writes to stdout and stderr immediately get added to a buffer that's flushed by the JS side on its own schedule,
// and Browser-side writes to stdin are read whenever the Tcl side requests them.
// **I haven't ever tested `chan event readable`.** That should be something that WaitForEvent
// tests, and queues, when stdin is readable.
// ... Or, alternatively, a write to stdin calls SurfTcl.AlertNotifier. Both are equally correct options.
//
// yeah no this can remain stubs for now. I'm keeping this comment here until I implement the actual channel API in the JS side.
// In particular, setting a `Tcl_FileProc` might begin to make sense, since the Tcl side might want to convene with the JS side
// when a channel representing a WebSocket is created. For example, to Tcl_QueueEvent when data is available.
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