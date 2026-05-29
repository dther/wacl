#include <tcl.h>
#include <string.h>
#include <stdint.h>
#include <emscripten.h>

#include "wacl.h"

/*
 * Wacl's JS bridge.
 *
 * The page (JS side) registers JS functions by name; the inner interp calls
 * them as `::wacl::js::call NAME ?ARG ...?`. Conventions:
 *
 *   - Args after NAME are passed varargs-style and arrive on the JS side
 *     as one array of strings. To pass a Tcl list as args, use
 *     `::wacl::js::call NAME {*}$myList`. Argument count and type checking
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

typedef int (*WaclJsFn)(int argc, const char **argv);

static Tcl_HashTable waclJsRegistry;
static int           waclJsRegistryInited = 0;

/* Result side channel — written by the JS shim, read by JsCallCmd. */
static Tcl_Obj *waclJsResultValue     = NULL;  /* owned ref, or NULL */
static Tcl_Obj *waclJsResultErrorCode = NULL;  /* owned ref, or NULL */

static void
waclJsResetResult(void)
{
    if (waclJsResultValue != NULL) {
        Tcl_DecrRefCount(waclJsResultValue);
        waclJsResultValue = NULL;
    }
    if (waclJsResultErrorCode != NULL) {
        Tcl_DecrRefCount(waclJsResultErrorCode);
        waclJsResultErrorCode = NULL;
    }
}

void
Wacl_SetJsResultString(const char *s)
{
    if (waclJsResultValue != NULL) Tcl_DecrRefCount(waclJsResultValue);
    waclJsResultValue = Tcl_NewStringObj(s ? s : "", -1);
    Tcl_IncrRefCount(waclJsResultValue);
}

void
Wacl_AppendJsErrorCodeElement(const char *s)
{
    if (waclJsResultErrorCode == NULL) {
        waclJsResultErrorCode = Tcl_NewListObj(0, NULL);
        Tcl_IncrRefCount(waclJsResultErrorCode);
    }
    Tcl_ListObjAppendElement(NULL, waclJsResultErrorCode,
                             Tcl_NewStringObj(s ? s : "", -1));
}

int
Wacl_RegisterJsFn(const char *name, int fnIdx)
{
    int isNew;
    Tcl_HashEntry *e = Tcl_CreateHashEntry(&waclJsRegistry, name, &isNew);
    Tcl_SetHashValue(e, (void *)(intptr_t) fnIdx);
    return isNew;
}

int
Wacl_RevokeJsFn(const char *name)
{
    Tcl_HashEntry *e = Tcl_FindHashEntry(&waclJsRegistry, name);
    if (e == NULL) return 0;
    Tcl_DeleteHashEntry(e);
    return 1;
}


static int
JsCallCmd(ClientData clientData, Tcl_Interp *interp,
          int objc, Tcl_Obj *const objv[])
{
    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "name ?arg ...?");
        return TCL_ERROR;
    }

    const char *name = Tcl_GetString(objv[1]);
    Tcl_HashEntry *e = Tcl_FindHashEntry(&waclJsRegistry, name);
    if (e == NULL) {
        Tcl_SetObjResult(interp, Tcl_ObjPrintf(
            "no such JS function: \"%s\"", name));
        Tcl_SetErrorCode(interp, "WACL", "JS", "NOTFOUND", (char *)NULL);
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

    waclJsResetResult();
    WaclJsFn fn = (WaclJsFn)(intptr_t) Tcl_GetHashValue(e);
    int rc = fn(argc, argv);

    if (argv != NULL) Tcl_Free((void *) argv);

    if (waclJsResultValue != NULL) {
        Tcl_SetObjResult(interp, waclJsResultValue);
    } else {
        Tcl_ResetResult(interp);
    }
    if (rc != TCL_OK && waclJsResultErrorCode != NULL) {
        Tcl_SetObjErrorCode(interp, waclJsResultErrorCode);
    }
    return rc;
}

static int
JsNamesCmd(ClientData clientData, Tcl_Interp *interp,
           int objc, Tcl_Obj *const objv[])
{
    if (objc != 1) {
        Tcl_WrongNumArgs(interp, 1, objv, NULL);
        return TCL_ERROR;
    }
    Tcl_Obj *result = Tcl_NewListObj(0, NULL);
    Tcl_HashSearch search;
    for (Tcl_HashEntry *e = Tcl_FirstHashEntry(&waclJsRegistry, &search);
         e != NULL;
         e = Tcl_NextHashEntry(&search)) {
        Tcl_ListObjAppendElement(NULL, result,
            Tcl_NewStringObj((const char *) Tcl_GetHashKey(&waclJsRegistry, e), -1));
    }
    Tcl_SetObjResult(interp, result);
    return TCL_OK;
}


/*
 * ::wacl::dom attr|css selector key value
 *
 * The pre-tDom DOM op. Kept as-is for now; will be retired once tDom is
 * brought in and the equivalent ops are exposed as registered JS functions
 * scoped to a chosen root node.
 */
static int
DomCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[])
{
    if (objc != 5) {
        Tcl_WrongNumArgs(interp, 1, objv, "attr|css selector key val");
        return TCL_ERROR;
    }

    const char *action   = Tcl_GetString(objv[1]);
    const char *selector = Tcl_GetString(objv[2]);
    const char *key      = Tcl_GetString(objv[3]);
    const char *val      = Tcl_GetString(objv[4]);

    if (strcmp(action, "attr") != 0 && strcmp(action, "css") != 0) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("Action must be attr or css", -1));
        return TCL_ERROR;
    }

    int numChanged = EM_ASM_INT({
        var action   = UTF8ToString($0);
        var selector = UTF8ToString($1);
        var key      = UTF8ToString($2);
        var val      = UTF8ToString($3);
        var elts = document.querySelectorAll(selector);
        for (var i = 0; i < elts.length; i++) {
            if (action === "attr") {
                elts[i][key] = val;
            } else {
                elts[i].style[key] = val;
            }
        }
        return elts.length;
    }, action, selector, key, val);

    Tcl_SetObjResult(interp, Tcl_NewIntObj(numChanged));
    return TCL_OK;
}


int
Wacl_Init(Tcl_Interp *interp)
{
    if (!waclJsRegistryInited) {
        Tcl_InitHashTable(&waclJsRegistry, TCL_STRING_KEYS);
        waclJsRegistryInited = 1;
    }

    Tcl_CreateNamespace(interp, "::wacl",     NULL, NULL);
    Tcl_CreateNamespace(interp, "::wacl::js", NULL, NULL);

    Tcl_CreateObjCommand(interp, "::wacl::dom",       DomCmd,     NULL, NULL);
    Tcl_CreateObjCommand(interp, "::wacl::js::call",  JsCallCmd,  NULL, NULL);
    Tcl_CreateObjCommand(interp, "::wacl::js::names", JsNamesCmd, NULL, NULL);

    Tcl_PkgProvide(interp, "wacl", "1.0.0");
    return TCL_OK;
}


/*
 * Tcl 9 turned Tcl_Eval and Tcl_GetStringResult into header macros (the
 * former expands to Tcl_EvalEx, the latter to Tcl_GetString of the obj
 * result). The wacl JS bridge cwraps them by name, so we provide thin
 * wrappers under the original names — #undef the macros first.
 *
 * The Tcl_Eval wrapper also fences re-entrant calls from JS. JS is
 * single-threaded so timers/promises can't produce concurrent Eval
 * calls, but a synchronous chain — JS Eval -> Tcl puts -> FS.init
 * output sink -> JS Eval — IS possible and would have the two frames
 * share one interpreter's result, errorInfo, and package init state.
 * Tcl handles nested evaluation fine when *Tcl* drives it (after,
 * fileevent, command callbacks all go through Tcl_DoOneEvent /
 * Tcl_EvalObjEx, not through our wrapper); only the JS-imposed flavour
 * needs to be refused. The idiomatic workaround on the caller's side
 * is `after 0 [list ...]`, which queues the inner script to run when
 * the current evaluation stack unwinds.
 */
#undef Tcl_Eval
static int waclEvalDepth = 0;
int
Tcl_Eval(Tcl_Interp *interp, const char *script)
{
    if (waclEvalDepth > 0) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj(
            "wacl: re-entrant Tcl_Eval from JS is not supported; "
            "queue the call with `after 0 [list ...]` instead", -1));
        return TCL_ERROR;
    }
    waclEvalDepth++;
    int rc = Tcl_EvalEx(interp, script, TCL_INDEX_NONE, 0);
    waclEvalDepth--;
    return rc;
}

#undef Tcl_GetStringResult
const char *
Tcl_GetStringResult(Tcl_Interp *interp)
{
    return Tcl_GetString(Tcl_GetObjResult(interp));
}
