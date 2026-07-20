/*
 * surftclChan.c — the surftcl channel: a byte FIFO between the page and the
 * interpreter, implemented as a C channel type.
 *
 * Tcl side:   surftcl::chan open NAME   -> a normal binary Tcl channel
 *             surftcl::chan names
 * JS side:    Runtime.chan.attach(NAME) -> { onData, write, close }
 *             (js/surftcl-bootstrap.mjs; reaches C via the exported
 *             SurfTcl_Chan* entry points below)
 *
 * Bytes cross the boundary as pointer+length, so the channel is byte-clean
 * including NUL. JS->Tcl bytes queue here in wasm memory; Tcl->JS bytes are
 * handed to the page synchronously from the output proc. Readability is
 * delivered by queueing a Tcl event (Tcl_QueueEvent -> Tcl_NotifyChannel),
 * never by notifying synchronously from the JS write path: dispatch stays
 * on the event-loop pump (the rAF tick, or Tcl's own `update`), mirroring
 * how reflected-channel postevents behave.
 *
 * Trust story: a channel is inert until the page attaches. Tcl may open
 * channels freely — output to an unattached channel is dropped, and input
 * only ever holds what the page chose to write — so unlike the JS function
 * registry, this command needs no host grant.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <tcl.h>
#include <emscripten.h>
#include "surftcl.h"

/*
 * Channel names are restricted to [A-Za-z0-9_-], at most 63 bytes. The
 * guard is what makes a name unambiguous across every boundary it crosses:
 * EM_ASM, cwrap'd C strings, and the space-joined list SurfTcl_ChanNames
 * returns.
 */
#define CHAN_NAME_MAX 63

typedef struct ChunkNode {
    struct ChunkNode *next;
    Tcl_Size len;
    Tcl_Size off;               /* bytes [0, off) already consumed */
    unsigned char *bytes;
} ChunkNode;

typedef struct SurfTclChan {
    char name[CHAN_NAME_MAX + 1];
    Tcl_Channel chan;
    ChunkNode *head, *tail;     /* pending JS->Tcl bytes */
    int watchMask;
    int notifyPosted;           /* a notify event is already queued */
    int jsClosed;               /* JS called close(): EOF after drain */
} SurfTclChan;

typedef struct ChanNotifyEvent {
    Tcl_Event header;
    char name[CHAN_NAME_MAX + 1];
} ChanNotifyEvent;

static Tcl_HashTable chanRegistry;
static int chanRegistryInited = 0;

static SurfTclChan *
LookupChan(const char *name)
{
    Tcl_HashEntry *entry;

    if (!chanRegistryInited) {
        return NULL;
    }
    entry = Tcl_FindHashEntry(&chanRegistry, name);
    return entry ? (SurfTclChan *) Tcl_GetHashValue(entry) : NULL;
}

/*
 * The queued event carries the channel NAME, not the record pointer: the
 * channel may close between queueing and delivery, and the re-lookup makes
 * that case a no-op instead of a use-after-free. A same-named channel
 * opened in the interim gets a spurious readable, which Tcl permits.
 */
static int
ChanNotifyEventProc(Tcl_Event *evPtr, int flags)
{
    ChanNotifyEvent *ev = (ChanNotifyEvent *) evPtr;
    SurfTclChan *rec;
    int mask = 0;

    if (!(flags & TCL_FILE_EVENTS)) {
        return 0;
    }
    rec = LookupChan(ev->name);
    if (rec != NULL) {
        rec->notifyPosted = 0;
        if ((rec->watchMask & TCL_READABLE)
                && (rec->head != NULL || rec->jsClosed)) {
            mask |= TCL_READABLE;
        }
        if (rec->watchMask & TCL_WRITABLE) {
            mask |= TCL_WRITABLE;   /* the page always accepts bytes */
        }
        if (mask) {
            Tcl_NotifyChannel(rec->chan, mask);
        }
    }
    return 1;
}

static void
QueueNotify(SurfTclChan *rec)
{
    ChanNotifyEvent *ev;

    if (rec->notifyPosted) {
        return;
    }
    rec->notifyPosted = 1;
    ev = (ChanNotifyEvent *) Tcl_Alloc(sizeof(ChanNotifyEvent));
    ev->header.proc = ChanNotifyEventProc;
    strcpy(ev->name, rec->name);
    Tcl_QueueEvent(&ev->header, TCL_QUEUE_TAIL);
}

/* ------------------------------------------------------ channel driver */

static int
ChanInput(void *instanceData, char *buf, int toRead, int *errorCodePtr)
{
    SurfTclChan *rec = (SurfTclChan *) instanceData;
    int copied = 0;

    while (copied < toRead && rec->head != NULL) {
        ChunkNode *ck = rec->head;
        Tcl_Size take = ck->len - ck->off;

        if (take > toRead - copied) {
            take = toRead - copied;
        }
        memcpy(buf + copied, ck->bytes + ck->off, take);
        ck->off += take;
        copied += take;
        if (ck->off == ck->len) {
            rec->head = ck->next;
            if (rec->head == NULL) {
                rec->tail = NULL;
            }
            Tcl_Free(ck->bytes);
            Tcl_Free(ck);
        }
    }
    if (copied == 0) {
        if (rec->jsClosed) {
            return 0;           /* drained and closed: real EOF */
        }
        *errorCodePtr = EAGAIN; /* empty but open: blocked, not EOF */
        return -1;
    }
    if ((rec->head != NULL || rec->jsClosed)
            && (rec->watchMask & TCL_READABLE)) {
        QueueNotify(rec);       /* leftovers keep the fileevent live */
    }
    return copied;
}

static int
ChanOutput(void *instanceData, const char *buf, int toWrite, int *errorCodePtr)
{
    SurfTclChan *rec = (SurfTclChan *) instanceData;

    (void) errorCodePtr;
    /*
     * _deliver catches its own callback errors and routes them through
     * Runtime.onError; the guard here only covers the surface not existing
     * yet (a write before postRun). Either way nothing may unwind Tcl's C
     * frames mid-write. An unattached channel simply drops the bytes.
     */
    EM_ASM({
        try {
            var R = Module.SurfTcl;
            if (R && R.chan) R.chan._deliver(UTF8ToString($0), $1, $2);
        } catch (e) {}
    }, rec->name, buf, toWrite);
    return toWrite;
}

static void
ChanWatch(void *instanceData, int mask)
{
    SurfTclChan *rec = (SurfTclChan *) instanceData;

    rec->watchMask = mask;
    if (((mask & TCL_READABLE) && (rec->head != NULL || rec->jsClosed))
            || (mask & TCL_WRITABLE)) {
        QueueNotify(rec);
    }
}

static int
ChanClose(void *instanceData, Tcl_Interp *interp, int flags)
{
    SurfTclChan *rec = (SurfTclChan *) instanceData;
    Tcl_HashEntry *entry;

    (void) interp;
    if (flags != 0) {
        return EINVAL;          /* no half-close */
    }
    EM_ASM({
        try {
            var R = Module.SurfTcl;
            if (R && R.chan) R.chan._closed(UTF8ToString($0));
        } catch (e) {}
    }, rec->name);
    entry = Tcl_FindHashEntry(&chanRegistry, rec->name);
    if (entry != NULL) {
        Tcl_DeleteHashEntry(entry);
    }
    while (rec->head != NULL) {
        ChunkNode *ck = rec->head;
        rec->head = ck->next;
        Tcl_Free(ck->bytes);
        Tcl_Free(ck);
    }
    Tcl_Free(rec);
    return 0;
}

static int
ChanBlockMode(void *instanceData, int mode)
{
    /*
     * Accept both modes; behavior is non-blocking either way. Reads on an
     * empty-but-open queue report EAGAIN honestly, and the notifier cannot
     * wait, so declared-blocking would change nothing but the label.
     */
    (void) instanceData;
    (void) mode;
    return 0;
}

static const Tcl_ChannelType surftclChannelType = {
    "surftclchan",
    TCL_CHANNEL_VERSION_5,
    NULL,                       /* closeProc: not used any more */
    ChanInput,
    ChanOutput,
    NULL,                       /* seekProc: not used any more */
    NULL,                       /* setOptionProc */
    NULL,                       /* getOptionProc */
    ChanWatch,
    NULL,                       /* getHandleProc */
    ChanClose,                  /* close2Proc */
    ChanBlockMode,
    NULL,                       /* flushProc */
    NULL,                       /* handlerProc */
    NULL,                       /* wideSeekProc */
    NULL,                       /* threadActionProc */
    NULL,                       /* truncateProc */
};

/* ------------------------------------------------------- construction */

static int
ValidChanName(const char *name)
{
    size_t i;

    if (name[0] == '\0') {
        return 0;
    }
    for (i = 0; name[i] != '\0'; i++) {
        char c = name[i];

        if (i > CHAN_NAME_MAX - 1) {
            return 0;
        }
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                || (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            return 0;
        }
    }
    return 1;
}

static SurfTclChan *
CreateChan(const char *name, int mask)
{
    SurfTclChan *rec;
    Tcl_HashEntry *entry;
    char chanName[CHAN_NAME_MAX + 16];
    int isNew;

    rec = (SurfTclChan *) Tcl_Alloc(sizeof(SurfTclChan));
    memset(rec, 0, sizeof(SurfTclChan));
    strcpy(rec->name, name);
    snprintf(chanName, sizeof(chanName), "surftcl_%s", name);
    rec->chan = Tcl_CreateChannel(&surftclChannelType, chanName, rec, mask);
    entry = Tcl_CreateHashEntry(&chanRegistry, rec->name, &isNew);
    Tcl_SetHashValue(entry, rec);
    return rec;
}

static int
surftcl_ChanOpenCmd(void *clientData, Tcl_Interp *interp,
                    int objc, Tcl_Obj *const objv[])
{
    const char *name;
    SurfTclChan *rec;

    (void) clientData;
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "name");
        return TCL_ERROR;
    }
    name = Tcl_GetString(objv[1]);
    if (!ValidChanName(name)) {
        Tcl_SetObjResult(interp, Tcl_ObjPrintf(
                "bad channel name \"%s\": must be [A-Za-z0-9_-], "
                "at most %d bytes", name, CHAN_NAME_MAX));
        Tcl_SetErrorCode(interp, "SURFTCL", "CHAN", "BADNAME", (char *) NULL);
        return TCL_ERROR;
    }
    if (LookupChan(name) != NULL) {
        Tcl_SetObjResult(interp, Tcl_ObjPrintf(
                "channel already open: %s", name));
        Tcl_SetErrorCode(interp, "SURFTCL", "CHAN", "EXISTS", (char *) NULL);
        return TCL_ERROR;
    }
    rec = CreateChan(name, TCL_READABLE | TCL_WRITABLE);
    Tcl_RegisterChannel(interp, rec->chan);
    Tcl_SetChannelOption(interp, rec->chan, "-translation", "binary");
    Tcl_SetChannelOption(interp, rec->chan, "-buffering", "none");
    Tcl_SetChannelOption(interp, rec->chan, "-blocking", "0");
    Tcl_SetObjResult(interp,
            Tcl_NewStringObj(Tcl_GetChannelName(rec->chan), -1));
    return TCL_OK;
}

static int
surftcl_ChanNamesCmd(void *clientData, Tcl_Interp *interp,
                     int objc, Tcl_Obj *const objv[])
{
    Tcl_Obj *list;
    Tcl_HashEntry *entry;
    Tcl_HashSearch search;

    (void) clientData;
    if (objc != 1) {
        Tcl_WrongNumArgs(interp, 1, objv, NULL);
        return TCL_ERROR;
    }
    list = Tcl_NewListObj(0, NULL);
    for (entry = Tcl_FirstHashEntry(&chanRegistry, &search);
            entry != NULL; entry = Tcl_NextHashEntry(&search)) {
        Tcl_ListObjAppendElement(interp, list, Tcl_NewStringObj(
                (const char *) Tcl_GetHashKey(&chanRegistry, entry), -1));
    }
    Tcl_SetObjResult(interp, list);
    return TCL_OK;
}

int
SurfTcl_ChanInit(Tcl_Interp *interp)
{
    if (!chanRegistryInited) {
        Tcl_InitHashTable(&chanRegistry, TCL_STRING_KEYS);
        chanRegistryInited = 1;
    }
    Tcl_CreateNamespace(interp, "::surftcl::chan", NULL, NULL);
    Tcl_CreateObjCommand(interp, "::surftcl::chan::open",
            surftcl_ChanOpenCmd, NULL, NULL);
    Tcl_CreateObjCommand(interp, "::surftcl::chan::names",
            surftcl_ChanNamesCmd, NULL, NULL);
    Tcl_Eval(interp, "namespace eval ::surftcl::chan"
            " {namespace export open names; namespace ensemble create}");
    /*
     * Provided by the runtime itself now, so it versions with the runtime
     * and `package require surftcl::chan` keeps working with no package
     * directory and no eval grant.
     */
    Tcl_PkgProvide(interp, "surftcl::chan", SURFTCL_VERSION);
    return TCL_OK;
}

/*
 * Install a surftcl channel as this thread's stdin, before anything
 * acquires the Emscripten fd-0 device (std channels are only ever
 * acquired lazily, so ordering is the whole trick). The channel sits in
 * the registry under the name "stdin"; Runtime.stdin in the bootstrap is
 * literally Runtime.chan.attach("stdin"). Text options mirror a terminal
 * stdin; the core underneath stays byte-oriented. If a script closes
 * stdin, Tcl falls back to lazily re-acquiring the fd-0 device — whose
 * honest read callback then reports EOF — rather than exploding.
 */
int
SurfTcl_InstallStdChannel(void)
{
    SurfTclChan *rec;

    if (!chanRegistryInited) {
        Tcl_InitHashTable(&chanRegistry, TCL_STRING_KEYS);
        chanRegistryInited = 1;
    }
    if (LookupChan("stdin") != NULL) {
        return TCL_ERROR;
    }
    rec = CreateChan("stdin", TCL_READABLE);
    Tcl_SetStdChannel(rec->chan, TCL_STDIN);
    Tcl_RegisterChannel(NULL, rec->chan);
    Tcl_SetChannelOption(NULL, rec->chan, "-translation", "auto");
    Tcl_SetChannelOption(NULL, rec->chan, "-encoding", "utf-8");
    Tcl_SetChannelOption(NULL, rec->chan, "-buffering", "line");
    Tcl_SetChannelOption(NULL, rec->chan, "-blocking", "0");
    return TCL_OK;
}

/* -------------------------------------------- JS-facing entry points */

int
SurfTcl_ChanWrite(const char *name, const unsigned char *buf, int len)
{
    SurfTclChan *rec = LookupChan(name);
    ChunkNode *ck;

    if (rec == NULL || rec->jsClosed || len < 0) {
        return -1;
    }
    if (len > 0) {
        ck = (ChunkNode *) Tcl_Alloc(sizeof(ChunkNode));
        ck->next = NULL;
        ck->len = len;
        ck->off = 0;
        ck->bytes = (unsigned char *) Tcl_Alloc(len);
        memcpy(ck->bytes, buf, len);
        if (rec->tail != NULL) {
            rec->tail->next = ck;
        } else {
            rec->head = ck;
        }
        rec->tail = ck;
        if (rec->watchMask & TCL_READABLE) {
            QueueNotify(rec);
        }
    }
    return len;
}

int
SurfTcl_ChanCloseFromJs(const char *name)
{
    SurfTclChan *rec = LookupChan(name);

    if (rec == NULL) {
        return -1;
    }
    rec->jsClosed = 1;          /* EOF once the queue drains */
    if (rec->watchMask & TCL_READABLE) {
        QueueNotify(rec);
    }
    return 0;
}

int
SurfTcl_ChanExists(const char *name)
{
    return LookupChan(name) != NULL;
}

const char *
SurfTcl_ChanNames(void)
{
    static Tcl_DString ds;
    static int dsInited = 0;
    Tcl_HashEntry *entry;
    Tcl_HashSearch search;

    if (dsInited) {
        Tcl_DStringFree(&ds);
    }
    Tcl_DStringInit(&ds);
    dsInited = 1;
    if (chanRegistryInited) {
        for (entry = Tcl_FirstHashEntry(&chanRegistry, &search);
                entry != NULL; entry = Tcl_NextHashEntry(&search)) {
            if (Tcl_DStringLength(&ds) > 0) {
                Tcl_DStringAppend(&ds, " ", 1);
            }
            Tcl_DStringAppend(&ds,
                    (const char *) Tcl_GetHashKey(&chanRegistry, entry), -1);
        }
    }
    return Tcl_DStringValue(&ds);
}
