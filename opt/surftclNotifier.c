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
        // DEFER(better errors) "would wait forever" is the default error,
	// but the caller would benefit from receiving a more detailed error message
	// that directs them to the docs.
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
