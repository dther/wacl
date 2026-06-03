#ifndef _SURFTCL_H_
#define _SURFTCL_H_

#include <tcl.h>

int SurfTcl_Init(Tcl_Interp *interp);

/*
 * JS bridge entry points, exported to the JS side via -sEXPORTED_FUNCTIONS
 * and reached through Module.cwrap. See opt/surftcl.c for the protocol.
 */
const char *SurfTcl_GetStringResult(Tcl_Interp *interp);
int  SurfTcl_RegisterJsFn(const char *name, int fnIdx);
int  SurfTcl_RevokeJsFn(const char *name);
int  SurfTcl_Eval(Tcl_Interp *interp, const char *name);
void SurfTcl_SetJsResultString(const char *s);
void SurfTcl_AppendJsErrorCodeElement(const char *s);

/*
 * Main-thread event-loop integration (opt/surftclNotifier.c). SurfTcl_InstallNotifier
 * swaps in a non-blocking notifier and must run before the notifier is first
 * used (call it at the top of main). SurfTcl_ServiceEvents is the JS-driven pump:
 * it drains all ready events without blocking and returns the count.
 */
void SurfTcl_InstallNotifier(void);
int  SurfTcl_ServiceEvents(void);

/*
 * Tcl-side `await`: yield to the JS event loop and resume in place. Backs
 * `::surftcl::js::yield` (and the Tcl `update` wrapper). Needs an Asyncify
 * build; see opt/surftclNotifier.c and docs/event-loop.md.
 */
int  SurfTcl_Yield(Tcl_Interp *interp);

#endif /* _SURFTCL_H_ */
