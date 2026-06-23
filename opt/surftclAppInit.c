#include <tcl.h>
#include <emscripten.h>
#include "surftcl.h"

/*
 * The main interpreter, 
 * initialized at startup and returned by SurfTcl_GetMainInterp
 */
static Tcl_Interp* mainInterp = NULL;

static int
SurfTcl_AppInit(Tcl_Interp* interp)
{
    // TODO(dther) I should merge these files. This literally will not work without the notifier.
    if (SurfTcl_Init(interp) != TCL_OK)
        printf("Error while initializing SurfTcl! Package will not be present");
    return 0;
}

Tcl_Interp*
SurfTcl_GetInterp()
{
    return mainInterp;
}

int
main(int argc, char** argv)
{
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

    // Finally, enter the event loop.
    //Tcl_MainEx(argc, argv, SurfTcl_AppInit, mainInterp);
    // ... Okay, problem. The above runs. *HOWEVER,* it exits immediately instead of entering the event loop.
    // I don't precisely know why, but the stack trace says that it gets as far as calling `Tcl_EvalObjEx("exit %d"...)`,
    // and does so with "0", meaning that it wasn't due to an error.
    // The most likely explanation is that it called `goto done`, likely because it fell out of the loop because it looked like stdin was EOF.
    // I've confirmed(?) this by pre-seeding stdin with a command to print to stdout, and it did so, then immediately exited.
    // We should probably roll the main loop ourselves. The question is... How?
    SurfTcl_AppInit(mainInterp);
    while (1) { Tcl_DoOneEvent(TCL_ALL_EVENTS); };
    // the above successfully suspends!!! Mind you, it doesn't read commands from stdin like it should, but it *works!*
    // I tested it in the JS command line via `__interp.Eval("after 1000 {puts hi}"); __interp.Module.__surftcl_wake()`.
    // TODO(dther) write a Tcl_Channel implementation because wrangling Tcl_Main and Emscripten's Unixy assumptions is rapidly diminishing returns
    // when neither teletypes, sockets, nor file descriptors "actually" exist inside a browser.
    // they can all just be Tcl_Channels with a flag in the JS somewhere, mayn
}
