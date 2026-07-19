#include <stdarg.h>
#include <stdio.h>
#include <tcl.h>
#include <emscripten.h>
#include "surftcl.h"

/*
 * The main interpreter,
 * initialized at startup and returned by SurfTcl_GetMainInterp
 */
static Tcl_Interp* mainInterp = NULL;

/*
 * Panic handler. Control must never return to Tcl_Panic — it would fall
 * through to __builtin_trap and the page would see an opaque wasm abort
 * ("Aborted()") with the message lost. Instead the EM_ASM throw unwinds
 * straight through the wasm frames into JS, carrying the message as a
 * TclPanic (or a plain Error if the module class isn't wired yet). The
 * abandoned wasm stack means the runtime is dead afterwards: a panic is
 * unrecoverable by design, and reloading the page is the recovery path.
 *
 * The message buffer is static because a panic may be an allocator failing.
 */
static void
SurfTclPanicProc(const char *format, ...)
{
    static char msg[1024];
    va_list args;

    va_start(args, format);
    vsnprintf(msg, sizeof(msg), format, args);
    va_end(args);

    EM_ASM({
        var msg = UTF8ToString($0);
        var panic = new (Module.TclPanic ?? Error)(msg);
        try { Module.SurfTcl.onError("panic", panic); } catch (e) {}
        throw panic;
    }, msg);
}

static int
SurfTcl_AppInit(Tcl_Interp* interp)
{
    if (SurfTcl_Init(interp) != TCL_OK)
        printf("Error while initializing SurfTcl! Package will not be present");
    return 0;
}

Tcl_Interp*
SurfTcl_GetInterp()
{
    return mainInterp;
}

static void
SurfTclMainLoop()
{
    // TODO(dther) refactor this to flush the entire event queue.
    // It should be highly unlikely that Tcl events block the browser-
    // if they do, that's a signal that our event semantics are wrong.
    Tcl_DoOneEvent(TCL_DONT_WAIT|TCL_ALL_EVENTS);
}

int
main(int argc, char** argv)
{
    /*
     * The panic proc must be installed before Tcl_CreateInterp:
     * Tcl_SetPanicProc runs Tcl_InitSubsystems, which is the documented
     * ordering for it.
     */
    Tcl_SetPanicProc(SurfTclPanicProc);

    /*
     * Swap in the non-blocking main-thread notifier before anything touches
     * the notifier (it initialises lazily on first use). From here on Tcl
     * never blocks waiting for an event; the rAF main loop below pumps
     * Tcl_DoOneEvent. See opt/surftclNotifier.c.
     */
    SurfTcl_InstallNotifier();

    mainInterp = Tcl_CreateInterp();

    /*
     * Tcl 9's standard library lives in a zip file (libtcl9.0.3.zip) which
     * we --embed-file into the Emscripten virtual FS at /lib/tcl.zip. On a
     * native build Tcl finds this automatically via TclZipfs_AppHook because
     * the zip is appended to the executable or shared library; in wasm we
     * have no executable to append to, so we mount the embedded zip
     * explicitly into the zipfs volume and point tcl_library at it.
     *
     * Mount point //zipfs:/lib/tcl is ZIPFS_ZIP_MOUNT — the canonical
     * location for a Tcl-library zip. The zip's top-level directory is
     * "tcl_library/", so init.tcl lives at //zipfs:/lib/tcl/tcl_library/.
     */
    if (TclZipfs_Mount(NULL, "/lib/tcl.zip", "//zipfs:/lib/tcl", NULL) != TCL_OK) {
        printf("SurfTcl: failed to mount embedded tcl_library zip\n");
    }

    Tcl_SetVar(mainInterp, "tcl_library",
               "//zipfs:/lib/tcl/tcl_library", TCL_GLOBAL_ONLY);
    if (Tcl_Init(mainInterp) != TCL_OK)
    {
        const char* errInfo = Tcl_GetVar(mainInterp, "::errorInfo", TCL_GLOBAL_ONLY);
        printf("Error while calling Tcl_Init: %s", errInfo);
    }

    /*
     * The stdin device never blocks — an empty queue reads as EAGAIN (see
     * stdin in js/surftcl-bootstrap.mjs) — and the notifier cannot wait, so
     * blocking semantics are unsatisfiable on the main thread. Declare the
     * channel non-blocking so its contract matches its behavior: gets/read
     * on an empty-but-open queue return nothing with fblocked 1 and eof 0.
     */
    Tcl_Eval(mainInterp, "chan configure stdin -blocking 0");

    SurfTcl_AppInit(mainInterp);

    // TODO(dther) we want to load `main.tcl` if it's present inside the zipfs
    // after *all initialisation* is done. Reason being:

    // The manpage of zipfs states that when asked to append a zip file to a
    // Tclsh binary, `zipfs mkimg` passes `main.tcl` to the application by a
    // mechanism I don't yet know.
    // `main.tcl` must be able to assume that all the necessary machinery
    // to bootstrap from a fresh Tclsh process is present. Namely,
    // core libraries loaded, the file system is coherent, packages are indexed,
    // and commands needed to extend further are all ready to go.
    // i.e., everything `SurfTcl_AppInit` has to do.

    // I want "give SurfTcl a zip file that would work on a desktop"
    // to be the idiomatic way to package applications for it,
    // so it saves shimming to follow the same behaviour.

    emscripten_set_main_loop(SurfTclMainLoop, 0, 0);

    return 0;
}

