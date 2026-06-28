#include <tcl.h>
#include <string.h>
#include <stdint.h>
#include <emscripten.h>
#include <errno.h>

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
SurfTclJsResetResult(void)
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

    SurfTclJsResetResult();
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
 *
 * The C side just removes the hash entry. The JS-side `surftcl.js.revoke`
 * additionally frees the Emscripten function-table slot via
 * removeFunction; revoking from Tcl leaves the slot allocated until
 * either the page reloads or someone re-registers the same name
 * (which fires the JS-side cleanup path). Small live-only cost, never
 * a real leak.
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
    int removed = SurfTcl_RevokeJsFn(name);
    Tcl_SetObjResult(interp, Tcl_NewIntObj(removed));
    return TCL_OK;
}

/*
 * ::surftcl::dom attr|css selector key value
 *
 * The pre-tDom DOM op. Kept as-is for now; will be retired once tDom is
 * brought in and the equivalent ops are exposed as registered JS functions
 * scoped to a chosen root node.
 */
static int
surftcl_DomCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[])
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


/*
 * Tcl 9 turned Tcl_Eval and Tcl_GetStringResult into header macros (the
 * former expands to Tcl_EvalEx, the latter to Tcl_GetString of the obj
 * result). The surftcl JS bridge cwraps these instead.
 *
 * surftclEval is a global variable that tracks if a concurrent call has been made.
 * A yield is __not possible__ when this is anything but 0.
 */
int surftclEval = 0;
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
    surftclEval++;
    // TODO(dther) consider Tcl_SaveInterpState before this, then restore after.
    // This way, a background error will dump the full call stack
    // but the context that yielded for whatever reason still has a clean slate.
    int r = Tcl_EvalEx(interp, script, -1, TCL_EVAL_GLOBAL);
    surftclEval--;
    return r;
}


const char *
SurfTcl_GetStringResult(Tcl_Interp *interp)
{
    return Tcl_GetString(Tcl_GetObjResult(interp));
}

// -------------------- Channel code, still testing.

/* What do we want to keep track of, anyway?
 * Maybe the JS side should be the source of truth.
 */
typedef struct BridgeState {
    char handle[64];
    Tcl_Channel channel;
    int blocking;
    int outputClosed;
    int inputClosed;
} BridgeState;

int
SurfTclBridgeInput(void *instanceData, char *buf, int bufSize, int *errorCodePtr)
{
    BridgeState *chan = (BridgeState *)instanceData;
    int n = EM_ASM_INT({
        Module.bridgeChannels.get(UTF8ToString($0))._tcl.input($1, $2)
    }, &(chan->handle), buf, bufSize);

    if (n == -1) *errorCodePtr = EAGAIN; /* JS has no data for us right now */
    if (n ==  0) chan->inputClosed = 1; /* JS side is closed and won't reopen */
    return n;
}

int
SurfTclBridgeOutput(void *instanceData, const char *buf, int toWrite, int *errorCodePtr)
{
    BridgeState *chan = (BridgeState *)instanceData;
    int n = EM_ASM_INT({
        Module.bridgeChannels.get(UTF8ToString($0))._tcl.output($1, $2)
    }, &(chan->handle), buf, toWrite);

    if (n == -1) {
        *errorCodePtr = EPIPE; /* other side no longer wants data */
        chan->outputClosed = 1;
    }
    return n;
}

void
SurfTclBridgeWatch(void *state, int mask)
{
    /* noop because we're always watching */
    return;
}

int
SurfTclBridgeClose2(void *state, Tcl_Interp *interp, int flags)
{
    // TODO(dther) implement half-closing logic...
    // TODO(dther) this should delete it from the JS side too
    return EINVAL;
}

int
SurfTclBridgeBlockMode(void *state, int mode)
{
    // SurfTcl bridge channels cannot block because the browser would freeze
    if (mode == TCL_MODE_BLOCKING) {
        Tcl_SetErrno(ENOTSUP);
        return ENOTSUP;
    }
    // DEFER(wait-blocking) could theoretically simulate with surftcl_wait

    /* always non-blocking, nothing to do */
    return 0;
}

int
SurfTclBridgeHandler(void *state, int interestMask)
{
    // TODO(dther) what do I do here...
    return EINVAL;
}

void
SurfTcl_NotifyBridgeWritable(Tcl_Channel chan)
{
    Tcl_NotifyChannel(chan, TCL_WRITABLE);
    // FIXME this segfaults. I don't know why.
    //Tcl_AlertNotifier(NULL);
}

void
SurfTcl_NotifyBridgeReadable(Tcl_Channel chan)
{
    Tcl_NotifyChannel(chan, TCL_READABLE);
    // FIXME this segfaults. I don't know why.
    //Tcl_AlertNotifier(NULL);
}

static const Tcl_ChannelType SurfTclBridgeChannel = {
    "surftclbridge",
    TCL_CHANNEL_VERSION_5,
    NULL, /* unused */
    SurfTclBridgeInput,
    SurfTclBridgeOutput,
    NULL, /* unused */
    NULL, // DEFER(bridge channels) setOptionProc, don't have any yet
    NULL, // DEFER(bridge channels) getOptionProc, don't have any yet
    SurfTclBridgeWatch,
    NULL, // DEFER(bridge handles) I don't have a scheme for handles locked in
    SurfTclBridgeClose2,
    SurfTclBridgeBlockMode,
    NULL, /* reserved */
    SurfTclBridgeHandler,
    NULL, /* can't seek on a FIFO */
    NULL, // DEFER(threadActionProc) do I need this?
    NULL, // DEFER(truncateProc) what does this do?
};

EM_JS(int, SurfTclNewBridge, (const char *handle, Tcl_Channel chan),
{
    /* how do I even test if this worked? */
    Module._surftcl_new_bridge(UTF8ToString(handle), chan);
});

Tcl_Channel
SurfTclOpenBridge(Tcl_Interp *interp, const char *handle, int mask)
{
    BridgeState *state = (BridgeState *)Tcl_Alloc(sizeof *state);
    sprintf(state->handle, "%s", handle);

    state->blocking = 0;
    state->inputClosed = 0;
    state->outputClosed = 0;

    Tcl_Channel chan = Tcl_CreateChannel(&SurfTclBridgeChannel, handle, state, mask);
    state->channel = chan;

    Tcl_SetChannelOption(interp, chan, "-blocking", "0");
    Tcl_SetChannelOption(interp, chan, "-encoding", "binary"); // TODO make this work with UTF8...

    // register on the JS side
    SurfTclNewBridge(handle, chan);
    //Tcl_RegisterChannel(interp, chan);

    return chan;
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

    Tcl_CreateObjCommand(interp, "::surftcl::dom",        surftcl_DomCmd,      NULL, NULL);
    Tcl_CreateObjCommand(interp, "::surftcl::js::call",   surftcl_JsCallCmd,   NULL, NULL);
    Tcl_CreateObjCommand(interp, "::surftcl::js::names",  surftcl_JsNamesCmd,  NULL, NULL);
    Tcl_CreateObjCommand(interp, "::surftcl::js::revoke", surftcl_JsRevokeCmd, NULL, NULL);

    Tcl_Channel chan = SurfTclOpenBridge(interp, "asdf", TCL_READABLE | TCL_WRITABLE);
    Tcl_Write(chan, "lolol\n", 6);

    Tcl_PkgProvide(interp, "surftcl", "1.0.0");
    return TCL_OK;
}
