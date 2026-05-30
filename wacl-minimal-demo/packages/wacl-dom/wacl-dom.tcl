# wacl::dom — bind Tcl scripts to DOM events.
#
# Surface:
#
#   ::wacl::dom bind SELECTOR EVENT SCRIPT
#       Adds a listener via addEventListener (so it composes with any
#       other listeners — no interference). Returns an opaque handle.
#       SCRIPT runs in the global scope each time the event fires; inside
#       it, use `::wacl::dom event FIELD` to pull fields from the current
#       event object.
#
#   ::wacl::dom unbind HANDLE
#       Removes the listener that bind returned.
#
#   ::wacl::dom event FIELD
#       Looks up FIELD on the current event being dispatched, lazily.
#       FIELD is a dot-path, so:
#           [::wacl::dom event type]            -> "click"
#           [::wacl::dom event clientX]         -> "150"
#           [::wacl::dom event target.id]       -> "my-button"
#           [::wacl::dom event target.tagName]  -> "BUTTON"
#       Missing paths and non-primitive endpoints return the empty string.
#       Outside a handler, returns "".
#
# The current-event slot is per-handler-frame: nested binds (rare but
# possible) save and restore it around their inner script. The Eval-fence
# in opt/wacl.c is *not* triggered by DOM events because they fire from
# the JS event loop between Tcl_Eval calls — the fence is for synchronous
# JS→Tcl→JS→Tcl re-entry inside one stack, which DOM dispatch isn't.

if {[info commands ::wacl::js::names] eq ""} {
    error "wacl::dom requires the wacl JS bridge"
}
if {[lsearch -exact [::wacl::js::names] eval] < 0} {
    error "wacl::dom install requires the host to have granted `eval`"
}

namespace eval ::wacl::dom {
    namespace export bind unbind event
    namespace ensemble create
}

::wacl::js::call eval {
    if (!globalThis.__waclDom) {
        globalThis.__waclDom = { bindings: {}, counter: 0, currentEvent: null };
    }
    var D = globalThis.__waclDom;

    wacl.js.register("__wacl_dom_bind", function (args) {
        var selector = args[0], evType = args[1], script = args[2];
        var elt = document.querySelector(selector);
        if (!elt) return [["WACL", "DOM", "NOMATCH"], "no element matches " + selector];
        var handle = "wd" + (++D.counter);
        var listener = function (e) {
            var prev = D.currentEvent;
            D.currentEvent = e;
            try {
                wacl.Eval(script);
            } catch (err) {
                wacl.onError("dom handler (" + evType + " on " + selector + ")", err);
            } finally {
                D.currentEvent = prev;
            }
        };
        elt.addEventListener(evType, listener);
        D.bindings[handle] = { elt: elt, type: evType, listener: listener };
        return handle;
    });

    wacl.js.register("__wacl_dom_unbind", function (args) {
        var b = D.bindings[args[0]];
        if (!b) return "";
        b.elt.removeEventListener(b.type, b.listener);
        delete D.bindings[args[0]];
        return "";
    });

    wacl.js.register("__wacl_dom_event", function (args) {
        if (D.currentEvent === null) return "";
        var path = args[0].split(".");
        var cur = D.currentEvent;
        for (var i = 0; i < path.length; i++) {
            if (cur === null || cur === undefined) return "";
            cur = cur[path[i]];
        }
        if (cur === undefined || cur === null) return "";
        if (typeof cur === "object" || typeof cur === "function") return "";
        return String(cur);
    });
}

proc ::wacl::dom::bind {selector event script} {
    ::wacl::js::call __wacl_dom_bind $selector $event $script
}

proc ::wacl::dom::unbind {handle} {
    ::wacl::js::call __wacl_dom_unbind $handle
}

proc ::wacl::dom::event {field} {
    ::wacl::js::call __wacl_dom_event $field
}

package provide wacl::dom 1.0
