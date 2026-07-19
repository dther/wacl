#include <tcl.h>
#include <string.h>
#include <stdint.h>
#include <emscripten.h>

#include "surftcl.h"

/*
 * SurfTcl's JS bridge.
 *
 * The page (JS side) registers JS functions by name; the inner interp calls
 * them as `::surftcl::js::call NAME ?ARG ...?`. Conventions:
 *
 *   - Args after NAME are passed varargs-style and arrive on the JS side
 *     as one array of strings. To pass a Tcl list as args, use
 *     `::surftcl::js::call NAME {*}$myList`. Argument count and type checking
 *     happen on the JS side; the C bridge stays type-blind.
 *   - The JS function returns either a bare value (becomes the Tcl result
 *     with TCL_OK), a [status, value] pair, or throws an Error (TCL_ERROR
 *     with the message). The five-name status convention (`ok`, `error`,
 *     `return`, `break`, `continue`), numeric codes, and errorCode-list
 *     handling all live in the JS shim — by the time we hear about a
 *     result on this side it is already a numeric return code plus a
 *     value string plus (optionally) an errorCode list.
 *   - Communication of the value/errorCode happens via a side channel the
 *     shim fills before returning. The function's own return value is the
 *     Tcl status code as an int.
 *
 * Registry: name → function-table index. JS hands us an Emscripten table
 * index (from Module.addFunction); we stash it in a hash. There is no
 * Tcl-side `register`. The host grants what the inner interp may call,
 * then optionally revokes capabilities (`eval` being the obvious one)
 * before running untrusted code — the polite-guest model.
 */

typedef int (*SurfTclJsFn)(int argc, const char **argv);

static Tcl_HashTable surftclJsRegistry;
static int           surftclJsRegistryInited = 0;

/* Result side channel — written by the JS shim, read by surftcl_JsCallCmd. */
static Tcl_Obj *surftclJsResultValue     = NULL;  /* owned ref, or NULL */
static Tcl_Obj *surftclJsResultErrorCode = NULL;  /* owned ref, or NULL */

static void
surftcl_JsResetResult(void)
{
    if (surftclJsResultValue != NULL) {
        Tcl_DecrRefCount(surftclJsResultValue);
        surftclJsResultValue = NULL;
    }
    if (surftclJsResultErrorCode != NULL) {
        Tcl_DecrRefCount(surftclJsResultErrorCode);
        surftclJsResultErrorCode = NULL;
    }
}

void
SurfTcl_SetJsResultString(const char *s)
{
    if (surftclJsResultValue != NULL) Tcl_DecrRefCount(surftclJsResultValue);
    surftclJsResultValue = Tcl_NewStringObj(s ? s : "", -1);
    Tcl_IncrRefCount(surftclJsResultValue);
}

void
SurfTcl_AppendJsErrorCodeElement(const char *s)
{
    if (surftclJsResultErrorCode == NULL) {
        surftclJsResultErrorCode = Tcl_NewListObj(0, NULL);
        Tcl_IncrRefCount(surftclJsResultErrorCode);
    }
    Tcl_ListObjAppendElement(NULL, surftclJsResultErrorCode,
                             Tcl_NewStringObj(s ? s : "", -1));
}

int
SurfTcl_RegisterJsFn(const char *name, int fnIdx)
{
    int isNew;
    Tcl_HashEntry *e = Tcl_CreateHashEntry(&surftclJsRegistry, name, &isNew);
    Tcl_SetHashValue(e, (void *)(intptr_t) fnIdx);
    return isNew;
}

int
SurfTcl_RevokeJsFn(const char *name)
{
    Tcl_HashEntry *e = Tcl_FindHashEntry(&surftclJsRegistry, name);
    if (e == NULL) return 0;
    Tcl_DeleteHashEntry(e);
    return 1;
}


static int
surftcl_JsCallCmd(ClientData clientData, Tcl_Interp *interp,
          int objc, Tcl_Obj *const objv[])
{
    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "name ?arg ...?");
        return TCL_ERROR;
    }

    // DEFER(js load testing) A hash lookup per JS call is a potential bottleneck.
    const char *name = Tcl_GetString(objv[1]);
    Tcl_HashEntry *e = Tcl_FindHashEntry(&surftclJsRegistry, name);
    if (e == NULL) {
        Tcl_SetObjResult(interp, Tcl_ObjPrintf(
            "no such JS function: \"%s\"", name));
        Tcl_SetErrorCode(interp, "SURFTCL", "JS", "NOTFOUND", (char *)NULL);
        return TCL_ERROR;
    }

    int argc = objc - 2;
    const char **argv = NULL;
    if (argc > 0) {
        argv = (const char **) Tcl_Alloc(argc * sizeof(char *));
        for (int i = 0; i < argc; i++) {
            argv[i] = Tcl_GetString(objv[i + 2]);
        }
    }

    surftcl_JsResetResult();
    SurfTclJsFn fn = (SurfTclJsFn)(intptr_t) Tcl_GetHashValue(e);
    int rc = fn(argc, argv);

    if (argv != NULL) Tcl_Free((void *) argv);

    if (surftclJsResultValue != NULL) {
        Tcl_SetObjResult(interp, surftclJsResultValue);
    } else {
        Tcl_ResetResult(interp);
    }
    if (rc != TCL_OK && surftclJsResultErrorCode != NULL) {
        Tcl_SetObjErrorCode(interp, surftclJsResultErrorCode);
    }
    return rc;
}

static int
surftcl_JsNamesCmd(ClientData clientData, Tcl_Interp *interp,
           int objc, Tcl_Obj *const objv[])
{
    if (objc != 1) {
        Tcl_WrongNumArgs(interp, 1, objv, NULL);
        return TCL_ERROR;
    }
    Tcl_Obj *result = Tcl_NewListObj(0, NULL);
    Tcl_HashSearch search;
    for (Tcl_HashEntry *e = Tcl_FirstHashEntry(&surftclJsRegistry, &search);
         e != NULL;
         e = Tcl_NextHashEntry(&search)) {
        Tcl_ListObjAppendElement(NULL, result,
            Tcl_NewStringObj((const char *) Tcl_GetHashKey(&surftclJsRegistry, e), -1));
    }
    Tcl_SetObjResult(interp, result);
    return TCL_OK;
}

/* ::surftcl::js::revoke NAME — remove an entry from the JS registry.
 *
 * Tcl can revoke but not register: revoke is voluntarily declining a
 * privilege the host granted, register would be expanding privilege.
 * A polite guest can do the former, never the latter. This is the
 * Tcl-side seal — bootstrap requires what it wants, then revokes
 * `eval` (and anything else broad) before user input lands.
 */
static int
surftcl_JsRevokeCmd(ClientData clientData, Tcl_Interp *interp,
            int objc, Tcl_Obj *const objv[])
{
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "name");
        return TCL_ERROR;
    }
    const char *name = Tcl_GetString(objv[1]);
    int removed = EM_ASM_INT({
        return Module.SurfTcl.js.revoke(Module.UTF8ToString($0));
    }, name);
    Tcl_SetObjResult(interp, Tcl_NewIntObj(removed));
    return TCL_OK;
}

int
SurfTcl_Init(Tcl_Interp *interp)
{
    if (!surftclJsRegistryInited) {
        Tcl_InitHashTable(&surftclJsRegistry, TCL_STRING_KEYS);
        surftclJsRegistryInited = 1;
    }

    Tcl_CreateNamespace(interp, "::surftcl",     NULL, NULL);
    Tcl_CreateNamespace(interp, "::surftcl::js", NULL, NULL);

    Tcl_CreateObjCommand(interp, "::surftcl::js::call",   surftcl_JsCallCmd,   NULL, NULL);
    Tcl_CreateObjCommand(interp, "::surftcl::js::names",  surftcl_JsNamesCmd,  NULL, NULL);
    Tcl_CreateObjCommand(interp, "::surftcl::js::revoke", surftcl_JsRevokeCmd, NULL, NULL);

    Tcl_PkgProvide(interp, "surftcl", SURFTCL_VERSION);
    return TCL_OK;
}


/*
 * Tcl 9 turned Tcl_Eval and Tcl_GetStringResult into header macros (the
 * former expands to Tcl_EvalEx, the latter to Tcl_GetString of the obj
 * result). The surftcl JS bridge cwraps these instead.
 *
 * SurfTcl_Eval is a thin wrapper — no re-entrancy fence. JS and Tcl share one
 * thread and cooperate on one event loop, so a JS callback invoked mid-Tcl
 * (via ::surftcl::js::call) may call straight back into SurfTcl_Eval. That is
 * fine: Tcl re-enters itself constantly — command substitution, `eval`,
 * `fileevent` callbacks — and is built for it. We deliberately do NOT
 * save/restore interpreter state around a nested call: a JS-side failure
 * that propagates should leave its errorInfo/errorCode intact, so the Tcl
 * side can `catch` it or let it bubble to the failure surface. Silent
 * isolation — papering over a nested error to keep frames "clean" — is the
 * one thing we reject. Runaway self-recursion is caught by Tcl's own
 * nesting limit ("too many nested evaluations (infinite loop?)"), a clean
 * Tcl error rather than a wasm stack overflow; we don't need our own wall.
 */
int
SurfTcl_Eval(Tcl_Interp *interp, const char *script)
{
    /*
     * TCL_EVAL_GLOBAL: a JS-initiated evaluation is a fresh top-level call
     * from outside, so it runs at global scope. Normally the current frame
     * already *is* global when JS calls in; it matters across a yield,
     * where the current frame is the parked evaluation's — without this, a
     * re-entrant SurfTcl_Eval during a yield would inherit that proc's locals.
     */
    return Tcl_EvalEx(interp, script, -1, TCL_EVAL_GLOBAL);
}


const char *
SurfTcl_GetStringResult(Tcl_Interp *interp)
{
    return Tcl_GetString(Tcl_GetObjResult(interp));
}
