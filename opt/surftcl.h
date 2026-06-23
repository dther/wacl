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
 * Main-thread event-loop integration (opt/surftclNotifier.c).
 * TODO(dther) explain better
 */
extern int surftclEval;
void SurfTcl_InstallNotifier(void);

#endif /* _SURFTCL_H_ */
