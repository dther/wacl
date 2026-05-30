# wacl::dom — bind Tcl scripts to DOM events, and manipulate the page
# directly. Consistently "trust JS to be JS": values cross the bridge as
# strings, dot-paths work for property access and method lookup, and
# there is no Tcl-side modelling of the DOM.
#
# Selectors are standard CSS (`#id`, `.class`, `div > p[data-x]`, …)
# and resolved through `document.querySelector`. Inside a `wacl::dom
# each` script, the special selector `:this` refers to the currently-
# iterated element.
#
# Events:
#
#   wacl::dom bind SELECTOR EVENT SCRIPT       -> handle
#       Adds a listener via addEventListener (composes — no
#       interference with other listeners). Returns an opaque handle.
#       SCRIPT runs each time the event fires; inside it,
#       `[wacl::dom event FIELD]` pulls fields off the current event.
#
#   wacl::dom unbind HANDLE
#       Removes the listener bind returned.
#
#   wacl::dom event FIELD                      -> string
#       Lazy dot-path lookup on the current event:
#           [wacl::dom event type]            -> "click"
#           [wacl::dom event clientX]         -> "150"
#           [wacl::dom event target.id]       -> "my-button"
#       Missing paths and non-primitive endpoints return the empty
#       string. Outside a handler, returns "".
#
# Properties, methods, content (all selector-driven):
#
#   wacl::dom prop  SEL PATH ?VALUE?           -> string | void
#       Get/set arbitrary properties via dot-path. PATH is a JS-style
#       dotted lookup: `id`, `dataset.userId`, `parentElement.id`, …
#       On get, primitive endpoints stringify; objects/functions/null
#       give "". On set, assigns to the last segment.
#
#   wacl::dom style SEL CSSPROP ?VALUE?        -> string | void
#       Get/set inline styles. CSSPROP can be CSS-hyphenated
#       (`background-color`) or camelCase (`backgroundColor`); the
#       getProperty/setProperty path accepts either.
#
#   wacl::dom call  SEL METHOD ?ARG ...?       -> string
#       Call a method by dot-path. ARGs are strings, passed verbatim.
#       Return is stringified; "" for undefined/null/objects.
#       Examples:
#           wacl::dom call #btn focus
#           wacl::dom call #btn classList.add highlighted
#           wacl::dom call body.firstElementChild scrollIntoView
#
#   wacl::dom html  SEL ?HTML?                 -> string | void
#       Get/set innerHTML. NOT sanitised — same XSS surface as
#       calling innerHTML directly. Use `text` for untrusted data.
#
#   wacl::dom text  SEL ?TEXT?                 -> string | void
#       Get/set textContent. Safe for untrusted data; the browser
#       handles all escaping.
#
#   wacl::dom before  SEL HTML
#       insertAdjacentHTML(beforebegin, …). Inserts HTML *before* the
#       element itself, as a previous sibling. Requires SEL to have
#       a parent.
#
#   wacl::dom prepend SEL HTML
#       insertAdjacentHTML(afterbegin, …). Inserts HTML as the first
#       child of SEL. Same XSS surface as `html`.
#
#   wacl::dom append  SEL HTML
#       insertAdjacentHTML(beforeend, …). Inserts HTML as the last
#       child of SEL. Same XSS surface as `html`.
#
#   wacl::dom after   SEL HTML
#       insertAdjacentHTML(afterend, …). Inserts HTML *after* the
#       element, as a next sibling. Requires SEL to have a parent.
#
#   wacl::dom remove SEL
#       elt.remove().
#
#   wacl::dom each   SEL BODY                  -> count
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

if {[info commands ::wacl::js::names] eq ""} {
    error "wacl::dom requires the wacl JS bridge"
}
if {[lsearch -exact [::wacl::js::names] eval] < 0} {
    error "wacl::dom install requires the host to have granted `eval`"
}

namespace eval ::wacl::dom {
    namespace export bind unbind event \
                     prop style call html text \
                     before prepend append after remove each
    namespace ensemble create
}

::wacl::js::call eval {
    if (!globalThis.__waclDom) {
        globalThis.__waclDom = {
            bindings: {},
            counter: 0,
            currentEvent: null,
            currentElement: null,
            elementStack: [],
            queryLists: {},
            qlCounter: 0
        };
        var D0 = globalThis.__waclDom;

        // Shared helpers. Stashed on the namespace object rather than
        // declared as globals so we don't pollute globalThis with bare
        // names like `D`, `resolveSel`, etc.

        D0.resolveSel = function (sel) {
            if (sel === ":this") {
                if (D0.currentElement === null) {
                    throw new Error(
                        "wacl::dom: `:this` is only valid inside `each`");
                }
                return D0.currentElement;
            }
            var elt = document.querySelector(sel);
            if (!elt) {
                var e = new Error("no element matches selector: " + sel);
                e.__waclCode = ["WACL", "DOM", "NOMATCH"];
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

    wacl.js.register("__wacl_dom_bind", function (args) {
        var D = globalThis.__waclDom;
        var selector = args[0], evType = args[1], script = args[2];
        var elt;
        try {
            elt = D.resolveSel(selector);
        } catch (e) {
            if (e.__waclCode) return [e.__waclCode, e.message];
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
                wacl.Eval(script);
            } catch (err) {
                wacl.onError(
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

    wacl.js.register("__wacl_dom_unbind", function (args) {
        var D = globalThis.__waclDom;
        var b = D.bindings[args[0]];
        if (!b) return "";
        b.elt.removeEventListener(b.type, b.listener);
        delete D.bindings[args[0]];
        return "";
    });

    wacl.js.register("__wacl_dom_event", function (args) {
        var D = globalThis.__waclDom;
        if (D.currentEvent === null) return "";
        return D.serialize(D.dotGet(D.currentEvent, args[0]));
    });

    wacl.js.register("__wacl_dom_prop", function (args) {
        var D = globalThis.__waclDom;
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
                    return [["WACL", "DOM", "NOPATH"],
                            "no such property path: " + args[1]];
                }
            }
            obj[parts[parts.length - 1]] = args[2];
            return "";
        } catch (e) {
            if (e.__waclCode) return [e.__waclCode, e.message];
            throw e;
        }
    });

    wacl.js.register("__wacl_dom_style", function (args) {
        var D = globalThis.__waclDom;
        try {
            var elt = D.resolveSel(args[0]);
            if (args.length === 2) {
                return elt.style.getPropertyValue(args[1]);
            }
            elt.style.setProperty(args[1], args[2]);
            return "";
        } catch (e) {
            if (e.__waclCode) return [e.__waclCode, e.message];
            throw e;
        }
    });

    wacl.js.register("__wacl_dom_call", function (args) {
        var D = globalThis.__waclDom;
        try {
            var elt = D.resolveSel(args[0]);
            var path = args[1];
            var methodArgs = args.slice(2);
            var parts = path.split(".");
            var receiver = elt;
            for (var i = 0; i < parts.length - 1; i++) {
                receiver = receiver[parts[i]];
                if (receiver === null || receiver === undefined) {
                    return [["WACL", "DOM", "NOPATH"],
                            "no such method path: " + path];
                }
            }
            var fn = receiver[parts[parts.length - 1]];
            if (typeof fn !== "function") {
                return [["WACL", "DOM", "NOPATH"],
                        "not a method: " + path];
            }
            return D.serialize(fn.apply(receiver, methodArgs));
        } catch (e) {
            if (e.__waclCode) return [e.__waclCode, e.message];
            throw e;
        }
    });

    wacl.js.register("__wacl_dom_html", function (args) {
        var D = globalThis.__waclDom;
        try {
            var elt = D.resolveSel(args[0]);
            if (args.length === 1) return elt.innerHTML;
            elt.innerHTML = args[1];
            return "";
        } catch (e) {
            if (e.__waclCode) return [e.__waclCode, e.message];
            throw e;
        }
    });

    wacl.js.register("__wacl_dom_text", function (args) {
        var D = globalThis.__waclDom;
        try {
            var elt = D.resolveSel(args[0]);
            if (args.length === 1) return elt.textContent;
            elt.textContent = args[1];
            return "";
        } catch (e) {
            if (e.__waclCode) return [e.__waclCode, e.message];
            throw e;
        }
    });

    // One primitive backing before / prepend / append / after.
    // Position is one of "beforebegin", "afterbegin", "beforeend",
    // "afterend" — the standard insertAdjacentHTML positions. The
    // Tcl-side procs pick the right one for their name.
    wacl.js.register("__wacl_dom_insert", function (args) {
        var D = globalThis.__waclDom;
        try {
            var elt = D.resolveSel(args[0]);
            elt.insertAdjacentHTML(args[1], args[2]);
            return "";
        } catch (e) {
            if (e.__waclCode) return [e.__waclCode, e.message];
            throw e;
        }
    });

    wacl.js.register("__wacl_dom_remove", function (args) {
        var D = globalThis.__waclDom;
        try {
            var elt = D.resolveSel(args[0]);
            elt.remove();
            return "";
        } catch (e) {
            if (e.__waclCode) return [e.__waclCode, e.message];
            throw e;
        }
    });

    // each is driven from Tcl-side: begin captures the NodeList under a
    // handle, step shifts currentElement to list[i], end restores the
    // previous currentElement and frees the list. The currentElement
    // stack handles nested each cleanly.

    wacl.js.register("__wacl_dom_each_begin", function (args) {
        var D = globalThis.__waclDom;
        var list = document.querySelectorAll(args[0]);
        var handle = "ql" + (++D.qlCounter);
        D.queryLists[handle] = list;
        D.elementStack.push(D.currentElement);
        return handle + " " + list.length;
    });

    wacl.js.register("__wacl_dom_each_step", function (args) {
        var D = globalThis.__waclDom;
        var list = D.queryLists[args[0]];
        if (!list) {
            return [["WACL", "DOM", "NOHANDLE"],
                    "no such query list: " + args[0]];
        }
        D.currentElement = list[parseInt(args[1], 10)];
        return "";
    });

    wacl.js.register("__wacl_dom_each_end", function (args) {
        var D = globalThis.__waclDom;
        delete D.queryLists[args[0]];
        D.currentElement = D.elementStack.pop();
        return "";
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

proc ::wacl::dom::prop {sel path args} {
    switch -- [llength $args] {
        0 { ::wacl::js::call __wacl_dom_prop $sel $path }
        1 { ::wacl::js::call __wacl_dom_prop $sel $path [lindex $args 0] }
        default {
            return -code error \
                "wrong # args: should be \"wacl::dom prop sel path ?value?\""
        }
    }
}

proc ::wacl::dom::style {sel prop args} {
    switch -- [llength $args] {
        0 { ::wacl::js::call __wacl_dom_style $sel $prop }
        1 { ::wacl::js::call __wacl_dom_style $sel $prop [lindex $args 0] }
        default {
            return -code error \
                "wrong # args: should be \"wacl::dom style sel cssprop ?value?\""
        }
    }
}

proc ::wacl::dom::call {sel method args} {
    ::wacl::js::call __wacl_dom_call $sel $method {*}$args
}

proc ::wacl::dom::html {sel args} {
    switch -- [llength $args] {
        0 { ::wacl::js::call __wacl_dom_html $sel }
        1 { ::wacl::js::call __wacl_dom_html $sel [lindex $args 0] }
        default {
            return -code error \
                "wrong # args: should be \"wacl::dom html sel ?html?\""
        }
    }
}

proc ::wacl::dom::text {sel args} {
    switch -- [llength $args] {
        0 { ::wacl::js::call __wacl_dom_text $sel }
        1 { ::wacl::js::call __wacl_dom_text $sel [lindex $args 0] }
        default {
            return -code error \
                "wrong # args: should be \"wacl::dom text sel ?text?\""
        }
    }
}

proc ::wacl::dom::before {sel html} {
    ::wacl::js::call __wacl_dom_insert $sel beforebegin $html
}

proc ::wacl::dom::prepend {sel html} {
    ::wacl::js::call __wacl_dom_insert $sel afterbegin $html
}

proc ::wacl::dom::append {sel html} {
    ::wacl::js::call __wacl_dom_insert $sel beforeend $html
}

proc ::wacl::dom::after {sel html} {
    ::wacl::js::call __wacl_dom_insert $sel afterend $html
}

proc ::wacl::dom::remove {sel} {
    ::wacl::js::call __wacl_dom_remove $sel
}

proc ::wacl::dom::each {sel body} {
    set info [::wacl::js::call __wacl_dom_each_begin $sel]
    lassign $info handle count
    try {
        for {set i 0} {$i < $count} {incr i} {
            ::wacl::js::call __wacl_dom_each_step $handle $i
            uplevel 1 $body
        }
    } finally {
        ::wacl::js::call __wacl_dom_each_end $handle
    }
    return $count
}

package provide wacl::dom 1.0
