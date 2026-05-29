#include <tcl.h>
#include <emscripten.h>
#include "wacl.h"

/* commentin' these out, we're just testing things.
EXTERN int Tdom_Init     _ANSI_ARGS_((Tcl_Interp *interp));
EXTERN int Tdom_SafeInit _ANSI_ARGS_((Tcl_Interp *interp));
EXTERN int Rl_json_Init             (Tcl_Interp *interp);
*/

/*
 * The main interpreter, 
 * initialized at startup and returned by Wacl_GetMainInterp
 */
static Tcl_Interp* mainInterp = NULL;

static int
Wacl_AppInit(Tcl_Interp* interp)
{
    if (Wacl_Init(interp) != TCL_OK)
        printf("Error while initialize Wacl! Package will not be present");
/*
    if (Tdom_Init(interp) != TCL_OK)
        printf("Error while initialize tDOM! Package will not be present");
    if (Rl_json_Init(interp) != TCL_OK)
        printf("Error while initialize rl_json! Package will not be present");
*/
    return 0;
}

static void
EmscriptenMainLoop()
{
    Tcl_DoOneEvent(TCL_DONT_WAIT|TCL_ALL_EVENTS);
}

Tcl_Interp*
Wacl_GetInterp()
{

    return mainInterp;
}

int
main(int argc, char** argv)
{
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
        printf("Wacl: failed to mount embedded tcl_library zip\n");
    }
    Tcl_SetVar(mainInterp, "tcl_library",
               "//zipfs:/lib/tcl/tcl_library", TCL_GLOBAL_ONLY);

    if (Tcl_Init(mainInterp) != TCL_OK)
    {
        const char* errInfo = Tcl_GetVar(mainInterp, "::errorInfo", TCL_GLOBAL_ONLY);
        printf("Error while calling Tcl_Init: %s", errInfo);
    }

    Wacl_AppInit(mainInterp);

    emscripten_set_main_loop(EmscriptenMainLoop,0,0);

    return 0;
}

