# surftcl::json — JSON syscalls into the browser.
#
# Surface (use as an ensemble — ::surftcl::json get ..., ::surftcl::json exists ...,
# or `namespace import ::surftcl::json` for the short `json get ...` form):
#
#   ::surftcl::json get $blob ?key ...?
#       Returns the value at the given path. Object keys are strings,
#       array indices are numeric (string form — Tcl convention).
#       null returns the empty string. If the value at the path is
#       itself an object or array, it comes back as a JSON string so
#       you can recurse with another `get`. Missing paths throw
#       TCL_ERROR with errorCode {JSON BAD_PATH}.
#
#   ::surftcl::json extract $blob ?key ...?
#       Like `get`, but returns the value as a JSON fragment — quoted
#       strings stay quoted, booleans stay bare, null stays bare.
#       The disambiguation lever for cases where `get` collapses (say)
#       the JSON string "true" and the boolean true to the same Tcl
#       representation. Mirrors rl_json's read-side disambiguator;
#       the same precedent will apply for any future typed-write
#       commands.
#
#   ::surftcl::json exists $blob ?key ...?
#       1 if the path is present (even if the value is null), 0 otherwise.
#
# We deliberately ship no `stringify` here. Tcl can't discriminate the
# string "true" from a boolean true — everything is a string — so building
# JSON from Tcl values is ambiguous in a way reading is not. For the
# reverse direction, escape via `::surftcl::js::call eval {JSON.stringify(...)}`
# until we have a proper Tcl-side JSON builder.
#
# Bootstrap: this package self-installs its JS shims at require time using
# the host-granted `eval`. So the host must have registered `eval` before
# `package require surftcl::json`. Intended sequence:
#     # page-side JS:   interp.js.register("eval", ...)
#     package require surftcl::json
#     package require surftcl::dom
#     ...
#     # page-side JS:   interp.js.revoke("eval")

if {[info commands ::surftcl::js::names] eq ""} {
    error "surftcl::json requires the surftcl JS bridge"
}
if {[lsearch -exact [::surftcl::js::names] eval] < 0} {
    error "surftcl::json install requires the host to have granted `eval`"
}

namespace eval ::surftcl::json {
    namespace export get extract exists
    namespace ensemble create
}

::surftcl::js::call eval {
    this.js.register("__surftcl_json_get", function (args) {
        var blob = args[0];
        var path = args.slice(1);
        var cur;
        try { cur = JSON.parse(blob); }
        catch (e) {
            return TclResult.error("json::get: " + e.message, { errorCode: ["JSON", "PARSE"] });
        }
        for (var i = 0; i < path.length; i++) {
            if (cur === null || typeof cur !== "object" || !(path[i] in cur)) {
                return TclResult.error("json::get: no such path: " + JSON.stringify(path.slice(0, i + 1)),
                    { errorCode: ["JSON", "BAD_PATH"] });
            }
            cur = cur[path[i]];
        }
        if (cur === null) return "";
        if (typeof cur === "object") return JSON.stringify(cur);
        return String(cur);
    });
    this.js.register("__surftcl_json_extract", function (args) {
        var blob = args[0];
        var path = args.slice(1);
        var cur;
        try { cur = JSON.parse(blob); }
        catch (e) {
            return TclResult.error("json::extract: " + e.message, { errorCode: ["JSON", "PARSE"] });
        }
        for (var i = 0; i < path.length; i++) {
            if (cur === null || typeof cur !== "object" || !(path[i] in cur)) {
                return TclResult.error("json::extract: no such path: " + JSON.stringify(path.slice(0, i + 1)),
                    { errorCode: ["JSON", "BAD_PATH"] });
            }
            cur = cur[path[i]];
        }
        return JSON.stringify(cur);
    });
    this.js.register("__surftcl_json_exists", function (args) {
        var blob = args[0];
        var path = args.slice(1);
        var cur;
        try { cur = JSON.parse(blob); }
        catch (e) {
             return TclResult.error("json::exists: " + e.message,
                 { errorCode: ["JSON", "PARSE"] });
        }
        for (var i = 0; i < path.length; i++) {
            if (cur === null || typeof cur !== "object") return "0";
            if (!(path[i] in cur)) return "0";
            cur = cur[path[i]];
        }
        return "1";
    });
}

proc ::surftcl::json::get {blob args} {
    ::surftcl::js::call __surftcl_json_get $blob {*}$args
}

proc ::surftcl::json::extract {blob args} {
    ::surftcl::js::call __surftcl_json_extract $blob {*}$args
}

proc ::surftcl::json::exists {blob args} {
    ::surftcl::js::call __surftcl_json_exists $blob {*}$args
}

package provide surftcl::json 0.0
