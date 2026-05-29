#ifndef _WACL_H_
#define _WACL_H_

#include <tcl.h>

int Wacl_Init(Tcl_Interp *interp);

/*
 * JS bridge entry points, exported to the JS side via -sEXPORTED_FUNCTIONS
 * and reached through Module.cwrap. See opt/wacl.c for the protocol.
 */
int  Wacl_RegisterJsFn(const char *name, int fnIdx);
int  Wacl_RevokeJsFn(const char *name);
void Wacl_SetJsResultString(const char *s);
void Wacl_AppendJsErrorCodeElement(const char *s);

#endif /* _WACL_H_ */
