# surftcl::dom — bind Tcl scripts to DOM events, and manipulate the page
# directly. Consistently "trust JS to be JS": values cross the bridge as
# strings, dot-paths work for property access and method lookup, and
# there is no Tcl-side modelling of the DOM.
#
# Selectors are standard CSS (`#id`, `.class`, `div > p[data-x]`, …)
# and resolved through `document.querySelector`. Inside a `surftcl::dom
# each` script, the special selector `:this` refers to the currently-
# iterated element.
#
# Events:
#
#   surftcl::dom bind SELECTOR EVENT SCRIPT       -> handle
#       Adds a listener via addEventListener (composes — no
#       interference with other listeners). Returns an opaque handle.
#       SCRIPT runs each time the event fires; inside it,
#       `[surftcl::dom event FIELD]` pulls fields off the current event.
#
#   surftcl::dom unbind HANDLE
#       Removes the listener bind returned.
#
#   surftcl::dom event FIELD                      -> string
#       Lazy dot-path lookup on the current event:
#           [surftcl::dom event type]            -> "click"
#           [surftcl::dom event clientX]         -> "150"
#           [surftcl::dom event target.id]       -> "my-button"
#       Missing paths and non-primitive endpoints return the empty
#       string. Outside a handler, returns "".
#
# Properties, methods, content (all selector-driven):
#
#   surftcl::dom prop  SEL PATH ?VALUE?           -> string | void
#       Get/set arbitrary properties via dot-path. PATH is a JS-style
#       dotted lookup: `id`, `dataset.userId`, `parentElement.id`, …
#       On get, primitive endpoints stringify; objects/functions/null
#       give "". On set, assigns to the last segment.
#
#   surftcl::dom style SEL CSSPROP ?VALUE?        -> string | void
#       Get/set inline styles. CSSPROP can be CSS-hyphenated
#       (`background-color`) or camelCase (`backgroundColor`); the
#       getProperty/setProperty path accepts either.
#
#   surftcl::dom call  SEL METHOD ?ARG ...?       -> string
#       Call a method by dot-path. ARGs are strings, passed verbatim.
#       Return is stringified; "" for undefined/null/objects.
#       Examples:
#           surftcl::dom call #btn focus
#           surftcl::dom call #btn classList.add highlighted
#           surftcl::dom call body.firstElementChild scrollIntoView
#
#   surftcl::dom html  SEL ?HTML?                 -> string | void
#       Get/set innerHTML. NOT sanitised — same XSS surface as
#       calling innerHTML directly. Use `text` for untrusted data.
#
#   surftcl::dom text  SEL ?TEXT?                 -> string | void
#       Get/set textContent. Safe for untrusted data; the browser
#       handles all escaping.
#
#   surftcl::dom before  SEL HTML
#       insertAdjacentHTML(beforebegin, …). Inserts HTML *before* the
#       element itself, as a previous sibling. Requires SEL to have
#       a parent.
#
#   surftcl::dom prepend SEL HTML
#       insertAdjacentHTML(afterbegin, …). Inserts HTML as the first
#       child of SEL. Same XSS surface as `html`.
#
#   surftcl::dom append  SEL HTML
#       insertAdjacentHTML(beforeend, …). Inserts HTML as the last
#       child of SEL. Same XSS surface as `html`.
#
#   surftcl::dom after   SEL HTML
#       insertAdjacentHTML(afterend, …). Inserts HTML *after* the
#       element, as a next sibling. Requires SEL to have a parent.
#
#   surftcl::dom remove SEL
#       elt.remove().
#
#   surftcl::dom each   SEL BODY                  -> count
#       Iterate all matches. BODY runs in the caller's scope
#       (uplevel 1), with `:this` resolving to the current element
#       inside any of the ops above. `break`, `continue`, `return`
#       from BODY do what their names say.
#
#       Iteration is driven from the Tcl side — each step issues
#       independent Tcl_Eval frames on the JS side rather than a
#       re-entrant call, so the eval-fence stays out of the way.
#
# Nesting: each + bind handlers + nested each all maintain proper
# stacks for current-event and current-element. The inner frame
# wins; the outer frame is restored on exit.

if {[info commands ::surftcl::js::names] eq ""} {
    error "surftcl::dom requires the surftcl JS bridge"
}
if {[lsearch -exact [::surftcl::js::names] eval] < 0} {
    error "surftcl::dom install requires the host to have granted `eval`"
}

namespace eval ::surftcl::dom {
    namespace export bind unbind event \
                     prop style call html text \
                     before prepend append after remove each
    namespace ensemble create
}

::surftcl::js::call eval {
    if (!globalThis.__surftclDom) {
        globalThis.__surftclDom = {
            bindings: {},
            counter: 0,
            currentEvent: null,
            currentElement: null,
            elementStack: [],
            queryLists: {},
            qlCounter: 0
        };
        var D0 = globalThis.__surftclDom;

        // Shared helpers. Stashed on the namespace object rather than
        // declared as globals so we don't pollute globalThis with bare
        // names like `D`, `resolveSel`, etc.

        D0.resolveSel = function (sel) {
            if (sel === ":this") {
                if (D0.currentElement === null) {
                    throw new Error(
                        "surftcl::dom: `:this` is only valid inside `each`");
                }
                return D0.currentElement;
            }
            var elt = document.querySelector(sel);
            if (!elt) {
                var e = new Error("no element matches selector: " + sel);
                e.__surftclCode = ["SURFTCL", "DOM", "NOMATCH"];
                throw e;
            }
            return elt;
        };

        D0.dotGet = function (obj, path) {
            var parts = path.split(".");
            for (var i = 0; i < parts.length; i++) {
                if (obj === null || obj === undefined) return undefined;
                obj = obj[parts[i]];
            }
            return obj;
        };

        D0.serialize = function (v) {
            if (v === undefined || v === null) return "";
            if (typeof v === "object" || typeof v === "function") return "";
            return String(v);
        };
    }

    surftcl.js.register("__surftcl_dom_bind", function (args) {
        var D = globalThis.__surftclDom;
        var selector = args[0], evType = args[1], script = args[2];
        var elt;
        try {
            elt = D.resolveSel(selector);
        } catch (e) {
            if (e.__surftclCode) return TclResult.error(e.message, { errorCode: e.__surftclCode });
            throw e;
        }
        var handle = "wd" + (++D.counter);
        var listener = function (e) {
            var prevEvent = D.currentEvent;
            var prevElement = D.currentElement;
            D.currentEvent = e;
            // The bound element becomes `:this` for the handler's body.
            // Same unification as `each` — current element is whatever the
            // current operation says it is; outside both it's null and
            // `:this` errors. Nested binds and each stack cleanly via the
            // save/restore here.
            D.currentElement = elt;
            try {
                surftcl.Eval(script);
            } catch (err) {
                surftcl.onError(
                    "dom handler (" + evType + " on " + selector + ")", err);
            } finally {
                D.currentEvent = prevEvent;
                D.currentElement = prevElement;
            }
        };
        elt.addEventListener(evType, listener);
        D.bindings[handle] = { elt: elt, type: evType, listener: listener };
        return handle;
    });

    surftcl.js.register("__surftcl_dom_unbind", function (args) {
        var D = globalThis.__surftclDom;
        var b = D.bindings[args[0]];
        if (!b) return "";
        b.elt.removeEventListener(b.type, b.listener);
        delete D.bindings[args[0]];
        return "";
    });

    surftcl.js.register("__surftcl_dom_event", function (args) {
        var D = globalThis.__surftclDom;
        if (D.currentEvent === null) return "";
        return D.serialize(D.dotGet(D.currentEvent, args[0]));
    });

    surftcl.js.register("__surftcl_dom_prop", function (args) {
        var D = globalThis.__surftclDom;
        try {
            var elt = D.resolveSel(args[0]);
            if (args.length === 2) {
                return D.serialize(D.dotGet(elt, args[1]));
            }
            // set: traverse to penultimate, assign last
            var parts = args[1].split(".");
            var obj = elt;
            for (var i = 0; i < parts.length - 1; i++) {
                obj = obj[parts[i]];
                if (obj === null || obj === undefined) {
                    return TclResult.error("no such property path: " + args[1], { errorCode: ["SURFTCL", "DOM", "NOPATH"] });
                }
            }
            obj[parts[parts.length - 1]] = args[2];
            return "";
        } catch (e) {
            if (e.__surftclCode) return TclResult.error(e.message, { errorCode: e.__surftclCode });
            throw e;
        }
    });

    surftcl.js.register("__surftcl_dom_style", function (args) {
        var D = globalThis.__surftclDom;
        try {
            var elt = D.resolveSel(args[0]);
            if (args.length === 2) {
                return elt.style.getPropertyValue(args[1]);
            }
            elt.style.setProperty(args[1], args[2]);
            return "";
        } catch (e) {
            if (e.__surftclCode) return TclResult.error(e.message, { errorCode: e.__surftclCode });
            throw e;
        }
    });

    surftcl.js.register("__surftcl_dom_call", function (args) {
        var D = globalThis.__surftclDom;
        try {
            var elt = D.resolveSel(args[0]);
            var path = args[1];
            var methodArgs = args.slice(2);
            var parts = path.split(".");
            var receiver = elt;
            for (var i = 0; i < parts.length - 1; i++) {
                receiver = receiver[parts[i]];
                if (receiver === null || receiver === undefined) {
                    return TclResult.error("no such method path: " + path, { errorCode: ["SURFTCL", "DOM", "NOPATH"] });
                }
            }
            var fn = receiver[parts[parts.length - 1]];
            if (typeof fn !== "function") {
                return TclResult.error("not a method: " + path, { errorCode: ["SURFTCL", "DOM", "NOPATH"] });
            }
            return D.serialize(fn.apply(receiver, methodArgs));
        } catch (e) {
            if (e.__surftclCode) return TclResult.error(e.message, { errorCode: e.__surftclCode });
            throw e;
        }
    });

    surftcl.js.register("__surftcl_dom_html", function (args) {
        var D = globalThis.__surftclDom;
        try {
            var elt = D.resolveSel(args[0]);
            if (args.length === 1) return elt.innerHTML;
            elt.innerHTML = args[1];
            return "";
        } catch (e) {
            if (e.__surftclCode) return TclResult.error(e.message, { errorCode: e.__surftclCode });
            throw e;
        }
    });

    surftcl.js.register("__surftcl_dom_text", function (args) {
        var D = globalThis.__surftclDom;
        try {
            var elt = D.resolveSel(args[0]);
            if (args.length === 1) return elt.textContent;
            elt.textContent = args[1];
            return "";
        } catch (e) {
            if (e.__surftclCode) return TclResult.error(e.message, { errorCode: e.__surftclCode });
            throw e;
        }
    });

    // One primitive backing before / prepend / append / after.
    // Position is one of "beforebegin", "afterbegin", "beforeend",
    // "afterend" — the standard insertAdjacentHTML positions. The
    // Tcl-side procs pick the right one for their name.
    surftcl.js.register("__surftcl_dom_insert", function (args) {
        var D = globalThis.__surftclDom;
        try {
            var elt = D.resolveSel(args[0]);
            elt.insertAdjacentHTML(args[1], args[2]);
            return "";
        } catch (e) {
            if (e.__surftclCode) return TclResult.error(e.message, { errorCode: e.__surftclCode });
            throw e;
        }
    });

    surftcl.js.register("__surftcl_dom_remove", function (args) {
        var D = globalThis.__surftclDom;
        try {
            var elt = D.resolveSel(args[0]);
            elt.remove();
            return "";
        } catch (e) {
            if (e.__surftclCode) return TclResult.error(e.message, { errorCode: e.__surftclCode });
            throw e;
        }
    });

    // each is driven from Tcl-side: begin captures the NodeList under a
    // handle, step shifts currentElement to list[i], end restores the
    // previous currentElement and frees the list. The currentElement
    // stack handles nested each cleanly.

    surftcl.js.register("__surftcl_dom_each_begin", function (args) {
        var D = globalThis.__surftclDom;
        var list = document.querySelectorAll(args[0]);
        var handle = "ql" + (++D.qlCounter);
        D.queryLists[handle] = list;
        D.elementStack.push(D.currentElement);
        return handle + " " + list.length;
    });

    surftcl.js.register("__surftcl_dom_each_step", function (args) {
        var D = globalThis.__surftclDom;
        var list = D.queryLists[args[0]];
        if (!list) {
            return TclResult.error("no such query list: " + args[0], { errorCode: ["SURFTCL", "DOM", "NOHANDLE"] });
        }
        D.currentElement = list[parseInt(args[1], 10)];
        return "";
    });

    surftcl.js.register("__surftcl_dom_each_end", function (args) {
        var D = globalThis.__surftclDom;
        delete D.queryLists[args[0]];
        D.currentElement = D.elementStack.pop();
        return "";
    });
}

proc ::surftcl::dom::bind {selector event script} {
    ::surftcl::js::call __surftcl_dom_bind $selector $event $script
}

proc ::surftcl::dom::unbind {handle} {
    ::surftcl::js::call __surftcl_dom_unbind $handle
}

proc ::surftcl::dom::event {field} {
    ::surftcl::js::call __surftcl_dom_event $field
}

proc ::surftcl::dom::prop {sel path args} {
    switch -- [llength $args] {
        0 { ::surftcl::js::call __surftcl_dom_prop $sel $path }
        1 { ::surftcl::js::call __surftcl_dom_prop $sel $path [lindex $args 0] }
        default {
            return -code error \
                "wrong # args: should be \"surftcl::dom prop sel path ?value?\""
        }
    }
}

proc ::surftcl::dom::style {sel prop args} {
    switch -- [llength $args] {
        0 { ::surftcl::js::call __surftcl_dom_style $sel $prop }
        1 { ::surftcl::js::call __surftcl_dom_style $sel $prop [lindex $args 0] }
        default {
            return -code error \
                "wrong # args: should be \"surftcl::dom style sel cssprop ?value?\""
        }
    }
}

proc ::surftcl::dom::call {sel method args} {
    ::surftcl::js::call __surftcl_dom_call $sel $method {*}$args
}

proc ::surftcl::dom::html {sel args} {
    switch -- [llength $args] {
        0 { ::surftcl::js::call __surftcl_dom_html $sel }
        1 { ::surftcl::js::call __surftcl_dom_html $sel [lindex $args 0] }
        default {
            return -code error \
                "wrong # args: should be \"surftcl::dom html sel ?html?\""
        }
    }
}

proc ::surftcl::dom::text {sel args} {
    switch -- [llength $args] {
        0 { ::surftcl::js::call __surftcl_dom_text $sel }
        1 { ::surftcl::js::call __surftcl_dom_text $sel [lindex $args 0] }
        default {
            return -code error \
                "wrong # args: should be \"surftcl::dom text sel ?text?\""
        }
    }
}

proc ::surftcl::dom::before {sel html} {
    ::surftcl::js::call __surftcl_dom_insert $sel beforebegin $html
}

proc ::surftcl::dom::prepend {sel html} {
    ::surftcl::js::call __surftcl_dom_insert $sel afterbegin $html
}

proc ::surftcl::dom::append {sel html} {
    ::surftcl::js::call __surftcl_dom_insert $sel beforeend $html
}

proc ::surftcl::dom::after {sel html} {
    ::surftcl::js::call __surftcl_dom_insert $sel afterend $html
}

proc ::surftcl::dom::remove {sel} {
    ::surftcl::js::call __surftcl_dom_remove $sel
}

proc ::surftcl::dom::each {sel body} {
    set info [::surftcl::js::call __surftcl_dom_each_begin $sel]
    lassign $info handle count
    try {
        for {set i 0} {$i < $count} {incr i} {
            ::surftcl::js::call __surftcl_dom_each_step $handle $i
            uplevel 1 $body
        }
    } finally {
        ::surftcl::js::call __surftcl_dom_each_end $handle
    }
    return $count
}

package provide surftcl::dom 0.0
