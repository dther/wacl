#ifndef _WACL_H_
#define _WACL_H_

#include <tcl.h>

int Wacl_Init(Tcl_Interp *interp);

/*
 * JS bridge entry points, exported to the JS side via -sEXPORTED_FUNCTIONS
 * and reached through Module.cwrap. See opt/wacl.c for the protocol.
 */
const char *Wacl_GetStringResult(Tcl_Interp *interp);
int  Wacl_RegisterJsFn(const char *name, int fnIdx);
int  Wacl_RevokeJsFn(const char *name);
int  Wacl_Eval(Tcl_Interp *interp, const char *name);
void Wacl_SetJsResultString(const char *s);
void Wacl_AppendJsErrorCodeElement(const char *s);

/*
 * Main-thread event-loop integration (opt/waclNotifier.c). Wacl_InstallNotifier
 * swaps in a non-blocking notifier and must run before the notifier is first
 * used (call it at the top of main). Wacl_ServiceEvents is the JS-driven pump:
 * it drains all ready events without blocking and returns the count.
 */
void Wacl_InstallNotifier(void);
int  Wacl_ServiceEvents(void);

#endif /* _WACL_H_ */
