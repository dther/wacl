#ifndef _SURFTCL_H_
#define _SURFTCL_H_

#include <tcl.h>

/*
 * The single source of truth for SurfTcl's version, in Tcl's own version
 * grammar (a = alpha, b = beta; see the `package` man page). Tcl_PkgProvide
 * uses it directly and the ES6 module derives its VERSION export from the
 * interp at boot, so nothing else needs updating when this changes.
 */
#define SURFTCL_VERSION "0.1a1"

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
void SurfTcl_Panic(const char *msg);

/*
 * The surftcl channel (opt/surftclChan.c): a C channel type whose JS side
 * is Runtime.chan in the bootstrap. SurfTcl_ChanInit registers the Tcl
 * command surface; the SurfTcl_Chan* entry points below are exported for
 * the JS attach surface (bytes cross as pointer+length).
 */
int SurfTcl_ChanInit(Tcl_Interp *interp);
int SurfTcl_InstallStdChannel(void);
int SurfTcl_ChanWrite(const char *name, const unsigned char *buf, int len);
int SurfTcl_ChanCloseFromJs(const char *name);
int SurfTcl_ChanExists(const char *name);
const char *SurfTcl_ChanNames(void);

/*
 * Main-thread event-loop integration (opt/surftclNotifier.c). SurfTcl_InstallNotifier
 * swaps in a non-blocking notifier and must run before the notifier is first
 * used (call it at the top of main).
 */
void SurfTcl_InstallNotifier(void);

#endif /* _SURFTCL_H_ */
