#include <tcl.h>
#include <string.h>
#include <stdarg.h>
#include <emscripten.h>

static const char* _valTypes[] = {
    "void",
    "array",
    "string", 
    "int", 
    "double", 
    "bool",
    (const char*)NULL
};

enum _valTypesEnum
{
    EMTCL_VOID,
    EMTCL_ARRAY,
    EMTCL_STRING,
    EMTCL_INT,
    EMTCL_DOUBLE,
    EMTCL_BOOL
};


static int 
DomCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[])
{
	char *argsHelp = "attr|css selector key val";
	if (objc != 5) {
		Tcl_WrongNumArgs(interp, 1, objv, argsHelp);
		return TCL_ERROR;
	}

	const char *action   = Tcl_GetString(objv[1]);
	const char *selector = Tcl_GetString(objv[2]);
	const char *key      = Tcl_GetString(objv[3]);
	const char *val      = Tcl_GetString(objv[4]);

	Tcl_Obj *res;

	if (strcmp(action, "attr") != 0 && strcmp(action, "css") != 0) {
		res = Tcl_NewStringObj("Action must be attr or css", -1);
		Tcl_SetObjResult(interp, res);
		return TCL_ERROR;
	}

	// TODO: always catch errors
	int numChanged = EM_ASM_INT({
		var action   = Pointer_stringify($0);
		    selector = Pointer_stringify($1);
		    key      = Pointer_stringify($2);
		    val      = Pointer_stringify($3);
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

	res = Tcl_NewIntObj(numChanged);
	Tcl_SetObjResult(interp, res);
	return TCL_OK;
}

#define EXPAND_FCN_CAST_CALL(R, X, ...) \
R (*fcn)(__VA_ARGS__) = ( R (*)(__VA_ARGS__))fcnPtr;\
R result;\
X

#define EXPAND_FCN_RET_TYPE(X, ...) \
switch (retTypeN)\
  {\
  case EMTCL_VOID:\
  {\
      EXPAND_FCN_CAST_CALL(int,X,__VA_ARGS__)\
      break;\
  }\
  case EMTCL_INT: case EMTCL_BOOL:\
  {\
      EXPAND_FCN_CAST_CALL(int,X,__VA_ARGS__)\
      Tcl_SetObjResult(interp, Tcl_NewIntObj(result));\
      break;\
  }\
  case EMTCL_ARRAY:\
  case EMTCL_STRING:\
    {\
      EXPAND_FCN_CAST_CALL(char*,X,__VA_ARGS__)\
      Tcl_SetObjResult(interp, Tcl_NewStringObj(result, -1));\
      break;\
    }\
  case EMTCL_DOUBLE:\
    {\
      EXPAND_FCN_CAST_CALL(double,X,__VA_ARGS__)\
      Tcl_SetObjResult(interp, Tcl_NewDoubleObj(result));\
      break;\
    }\
  default:\
    break;\
  }

#define EXPAND_FCN_ARG_TYPE(X) \
switch (argTypeN)\
{\
    case EMTCL_INT : case EMTCL_BOOL :\
    {\
      EXPAND_FCN_RET_TYPE(\
        int val;\
        Tcl_GetIntFromObj(interp, objv[4], &val);\
        X\
        , int\
      )\
      break;\
    }\
    case EMTCL_DOUBLE:\
    {\
      EXPAND_FCN_RET_TYPE(\
        double val;\
        Tcl_GetDoubleFromObj(interp, objv[4], &val);\
        X\
        , double\
      )\
      break;\
    }\
    case EMTCL_ARRAY: case EMTCL_STRING: default:\
    {\
      EXPAND_FCN_RET_TYPE(\
        const char* val = Tcl_GetString(objv[4]);\
        X\
        ,const char*\
      )\
      break;\
    }\
  }


static int 
JsCallCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[]) 
{
    int fcnPtr, retTypeN, argTypeN;

    if (objc < 4 || objc > 5)
    {
    Tcl_WrongNumArgs(
                    interp, 
                    1,
                    objv, 
                    "fcnPtr returnType argsTypes ?arg1 arg2 ...?");
    return TCL_ERROR;
    }

    if (Tcl_GetIntFromObj(interp, objv[1], &fcnPtr) != TCL_OK)
    {
      Tcl_SetObjResult(interp, Tcl_NewStringObj("first argument must be a function pointer", -1));
      return TCL_ERROR;
    }

    if (Tcl_GetIndexFromObj(interp, objv[2], _valTypes, "return type", TCL_EXACT, &retTypeN) != TCL_OK)
    return TCL_ERROR;

    if (Tcl_GetIndexFromObj(interp, objv[3], _valTypes, "argument type", TCL_EXACT, &argTypeN) != TCL_OK)
    return TCL_ERROR;

    if (argTypeN != EMTCL_VOID && objc != 5)
    {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("for void argument type there must be no argument", -1));
        return TCL_ERROR;
    }
    
    if (argTypeN == EMTCL_VOID)
    {
        EXPAND_FCN_RET_TYPE( result = fcn(); )
    }
    else
    {
        EXPAND_FCN_ARG_TYPE( result = fcn(val); )
    }
    
    return TCL_OK;
}


static void
WaclDeleteNamespace(ClientData clientData)
{
}

int
Wacl_Init(Tcl_Interp* interp)
{
    Tcl_Namespace* waclNs = NULL;
    
    /* commands initialization */
    waclNs = Tcl_CreateNamespace(interp, "::wacl", NULL, WaclDeleteNamespace);
    Tcl_CreateObjCommand(interp, 
                         "::wacl::dom", 
                         DomCmd, 
                         (ClientData) NULL, 
                         (Tcl_CmdDeleteProc *) NULL);
    Tcl_CreateObjCommand(interp, 
                         "::wacl::jscall", 
                         JsCallCmd, 
                         (ClientData) NULL, 
                         (Tcl_CmdDeleteProc *) NULL);

    Tcl_Export(interp, waclNs, "dom", 0);
    Tcl_Export(interp, waclNs, "jscall", 0);
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
