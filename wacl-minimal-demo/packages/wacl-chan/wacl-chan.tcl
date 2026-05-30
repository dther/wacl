# wacl::chan — bidirectional binary bytestreams between Tcl and JS,
# implemented as Tcl reflected channels (`chan create`). The channel
# behaves exactly like any other Tcl channel: puts/read/gets work,
# fileevent works, fconfigure works.
#
# This is the lever for #4 in the design discussion: a JS write does
# `chan postevent` on the Tcl side, which queues a readable wake-up on
# the normal Tcl event loop. The bytestream IS the event queue, with
# fileevent as the dispatch mechanism. A click handler that wants to
# notify Tcl, a fetch that wants to deliver a response, a WebSocket
# message that wants to land in a Tcl script — all the same shape.
#
# Surface (Tcl side):
#
#   ::wacl::chan open NAME
#       Creates a channel under NAME. Returns the Tcl channel name
#       (use it as you would any fd). Channel is binary, unbuffered.
#       Throws if NAME is already open.
#
#   ::wacl::chan names
#       List of names currently open.
#
# Surface (JS side, on globalThis.waclChan):
#
#   waclChan.attach(name) -> { onData, write, close }
#       Attach to an existing channel by name. Throws if no such name.
#       Returned object:
#         .onData = function(Uint8Array)   // called when Tcl writes to us
#         .write(stringOrUint8Array)        // push bytes for Tcl to read,
#                                           // wakes any fileevent readable
#         .close()                          // closes Tcl-side too
#
#   waclChan.names() -> [string,...]
#       Names currently open.
#
# Bytes round-trip via latin-1 across the bridge: a Uint8Array byte N
# becomes the JS string char.charCodeAt() == N, which Tcl receives as
# a Unicode codepoint N, which a binary-translation channel writes as
# the literal byte N. Identity preserved end-to-end.
#
# The JS-side write defers its `chan postevent` via setTimeout(0). DOM
# event handlers (and anything else that might call write inside an
# ongoing wacl.Eval frame) would otherwise trip the re-entrant
# Tcl_Eval fence; the setTimeout trampolines past the current Tcl
# stack the same way `after 0 [list ...]` does Tcl-side.

if {[info commands ::wacl::js::names] eq ""} {
    error "wacl::chan requires the wacl JS bridge"
}
if {[lsearch -exact [::wacl::js::names] eval] < 0} {
    error "wacl::chan install requires the host to have granted `eval`"
}

namespace eval ::wacl::chan {
    namespace export open names
    namespace ensemble create
    variable byName     ;# array: name -> jsHandle
    array set byName {}
}

::wacl::js::call eval {
    if (!globalThis.__waclChan) {
        globalThis.__waclChan = {
            byName: Object.create(null),   // name -> record
            counter: 0
        };
        var C = globalThis.__waclChan;

        // Bytes <-> latin1-string round-trip. Chunked to avoid the
        // String.fromCharCode.apply argument-count limit on big payloads.
        var CHUNK = 16384;
        function bytesToStr(bytes) {
            if (bytes.length <= CHUNK) {
                return String.fromCharCode.apply(null, bytes);
            }
            var parts = [];
            for (var i = 0; i < bytes.length; i += CHUNK) {
                parts.push(String.fromCharCode.apply(null,
                    bytes.subarray(i, Math.min(i + CHUNK, bytes.length))));
            }
            return parts.join("");
        }
        function strToBytes(s) {
            var b = new Uint8Array(s.length);
            for (var i = 0; i < s.length; i++) b[i] = s.charCodeAt(i) & 0xff;
            return b;
        }
        C.bytesToStr = bytesToStr;
        C.strToBytes = strToBytes;

        // Page-side attach. The Tcl side must already have called
        // `::wacl::chan open NAME` for this to work.
        globalThis.waclChan = {
            attach: function (name) {
                var rec = C.byName[name];
                if (!rec) throw new Error("wacl::chan: no such channel: " + name);
                return {
                    set onData(fn) { rec.onData = fn; },
                    get onData() { return rec.onData; },
                    write: function (data) {
                        if (rec.closed) throw new Error("wacl::chan: closed");
                        var bytes = (typeof data === "string")
                            ? new TextEncoder().encode(data)
                            : data;
                        rec.pending.push(bytes);
                        rec.pendingLen += bytes.length;
                        // Defer postevent past the current Tcl stack
                        // frame (if any) to dodge the eval-fence.
                        if (rec.watching && !rec.posted) {
                            rec.posted = true;
                            setTimeout(function () {
                                rec.posted = false;
                                if (rec.closed) return;
                                try {
                                    wacl.Eval(
                                        "chan postevent " + rec.tclName + " read");
                                } catch (e) {
                                    wacl.onError("chan postevent", e);
                                }
                            }, 0);
                        }
                    },
                    close: function () {
                        if (rec.closed) return;
                        rec.closed = true;
                        setTimeout(function () {
                            try { wacl.Eval("close " + rec.tclName); }
                            catch (e) { /* may already be closed */ }
                        }, 0);
                    }
                };
            },
            names: function () { return Object.keys(C.byName); }
        };
    }
    var C = globalThis.__waclChan;

    wacl.js.register("__wacl_chan_create", function (args) {
        var name = args[0];
        if (C.byName[name]) {
            return [["WACL", "CHAN", "EXISTS"], "channel already open: " + name];
        }
        var handle = "wc" + (++C.counter);
        C.byName[name] = {
            handle: handle,
            name: name,
            tclName: null,          // filled in by __wacl_chan_attach
            pending: [],            // Uint8Array chunks waiting for Tcl read
            pendingLen: 0,
            watching: false,
            posted: false,
            closed: false,
            onData: null
        };
        // Index by handle too for cheap lookup
        C.byName["__h_" + handle] = C.byName[name];
        return handle;
    });

    wacl.js.register("__wacl_chan_attach", function (args) {
        var handle = args[0], tclName = args[1];
        var rec = C.byName["__h_" + handle];
        if (!rec) return [["WACL", "CHAN", "NOHANDLE"], "no such chan handle"];
        rec.tclName = tclName;
        return "";
    });

    wacl.js.register("__wacl_chan_close", function (args) {
        var rec = C.byName["__h_" + args[0]];
        if (!rec) return "";
        rec.closed = true;
        delete C.byName[rec.name];
        delete C.byName["__h_" + rec.handle];
        return "";
    });

    wacl.js.register("__wacl_chan_watch", function (args) {
        var rec = C.byName["__h_" + args[0]];
        if (!rec) return "";
        rec.watching = (args[1].indexOf("read") >= 0);
        // If we're starting to watch and there's pending data, post
        // immediately so the fileevent fires.
        if (rec.watching && rec.pendingLen > 0 && !rec.posted) {
            rec.posted = true;
            setTimeout(function () {
                rec.posted = false;
                if (rec.closed) return;
                try { wacl.Eval("chan postevent " + rec.tclName + " read"); }
                catch (e) { wacl.onError("chan postevent", e); }
            }, 0);
        }
        return "";
    });

    wacl.js.register("__wacl_chan_read", function (args) {
        var rec = C.byName["__h_" + args[0]];
        var count = parseInt(args[1], 10);
        if (!rec || rec.pendingLen === 0) return "";
        // Concatenate pending into one buffer, slice, push remainder back.
        var all = new Uint8Array(rec.pendingLen);
        var off = 0;
        for (var i = 0; i < rec.pending.length; i++) {
            all.set(rec.pending[i], off);
            off += rec.pending[i].length;
        }
        rec.pending.length = 0;
        rec.pendingLen = 0;
        var take = Math.min(count, all.length);
        var chunk = all.subarray(0, take);
        var rest = all.subarray(take);
        if (rest.length > 0) {
            rec.pending.push(rest);
            rec.pendingLen = rest.length;
            // Still data left — keep the readable signal alive.
            if (rec.watching && !rec.posted) {
                rec.posted = true;
                setTimeout(function () {
                    rec.posted = false;
                    if (rec.closed) return;
                    try { wacl.Eval("chan postevent " + rec.tclName + " read"); }
                    catch (e) {}
                }, 0);
            }
        }
        return C.bytesToStr(chunk);
    });

    wacl.js.register("__wacl_chan_write", function (args) {
        var rec = C.byName["__h_" + args[0]];
        if (!rec) return [["WACL", "CHAN", "NOHANDLE"], "no such chan handle"];
        if (rec.closed) return [["WACL", "CHAN", "CLOSED"], "channel closed"];
        var bytes = C.strToBytes(args[1]);
        if (rec.onData) {
            try { rec.onData(bytes); }
            catch (e) { wacl.onError("chan onData", e); }
        }
        return String(bytes.length);
    });
}

# The reflected-channel handler. The list-prefix passed to chan create
# carries our private JS handle as $handle, so the runtime calls us as
#   handler $handle SUBCMD CHANID ?args?
proc ::wacl::chan::handler {handle cmd chanid args} {
    switch -- $cmd {
        initialize {
            # args = {modeList}
            return {initialize finalize watch read write}
        }
        finalize {
            ::wacl::js::call __wacl_chan_close $handle
            variable byName
            foreach name [array names byName] {
                if {$byName($name) eq $handle} { unset byName($name); break }
            }
            return
        }
        watch {
            ::wacl::js::call __wacl_chan_watch $handle [lindex $args 0]
            return
        }
        read {
            set count [lindex $args 0]
            set data [::wacl::js::call __wacl_chan_read $handle $count]
            if {$data eq ""} {
                return -code error EAGAIN
            }
            return $data
        }
        write {
            set data [lindex $args 0]
            ::wacl::js::call __wacl_chan_write $handle $data
            return [string length $data]
        }
    }
}

proc ::wacl::chan::open {name} {
    variable byName
    if {[info exists byName($name)]} {
        return -code error -errorcode {WACL CHAN EXISTS} \
            "channel already open: $name"
    }
    set handle [::wacl::js::call __wacl_chan_create $name]
    set ch [chan create {read write} \
                [list ::wacl::chan::handler $handle]]
    ::wacl::js::call __wacl_chan_attach $handle $ch
    fconfigure $ch -translation binary -buffering none -blocking 0
    set byName($name) $handle
    return $ch
}

proc ::wacl::chan::names {} {
    variable byName
    return [array names byName]
}

package provide wacl::chan 1.0
