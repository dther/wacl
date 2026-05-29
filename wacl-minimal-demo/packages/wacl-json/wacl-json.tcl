# wacl::json — JSON syscalls into the browser.
#
# Surface (use as an ensemble — ::wacl::json get ..., ::wacl::json exists ...,
# or `namespace import ::wacl::json` for the short `json get ...` form):
#
#   ::wacl::json get $blob ?key ...?
#       Returns the value at the given path. Object keys are strings,
#       array indices are numeric (string form — Tcl convention).
#       Missing fields, null, and undefined all return the empty string.
#       If the value at the path is itself an object or array, it comes
#       back as a JSON string so you can recurse with another `get`.
#
#   ::wacl::json exists $blob ?key ...?
#       1 if the path is present (even if the value is null), 0 otherwise.
#
# We deliberately ship no `stringify` here. Tcl can't discriminate the
# string "true" from a boolean true — everything is a string — so building
# JSON from Tcl values is ambiguous in a way reading is not. For the
# reverse direction, escape via `::wacl::js::call eval {JSON.stringify(...)}`
# until we have a proper Tcl-side JSON builder.
#
# Bootstrap: this package self-installs its JS shims at require time using
# the host-granted `eval`. So the host must have registered `eval` before
# `package require wacl::json`. Intended sequence:
#     # page-side JS:   interp.js.register("eval", ...)
#     package require wacl::json
#     package require wacl::dom
#     ...
#     # page-side JS:   interp.js.revoke("eval")

if {[info commands ::wacl::js::names] eq ""} {
    error "wacl::json requires the wacl JS bridge"
}
if {[lsearch -exact [::wacl::js::names] eval] < 0} {
    error "wacl::json install requires the host to have granted `eval`"
}

namespace eval ::wacl::json {
    namespace export get exists
    namespace ensemble create
}

::wacl::js::call eval {
    __interp.js.register("__wacl_json_get", function (args) {
        var blob = args[0];
        var path = args.slice(1);
        var cur;
        try { cur = JSON.parse(blob); }
        catch (e) { return [["JSON", "PARSE"], "json::get: " + e.message]; }
        for (var i = 0; i < path.length; i++) {
            if (cur === null || cur === undefined) return "";
            cur = cur[path[i]];
        }
        if (cur === undefined || cur === null) return "";
        if (typeof cur === "object") return JSON.stringify(cur);
        return String(cur);
    });
    __interp.js.register("__wacl_json_exists", function (args) {
        var blob = args[0];
        var path = args.slice(1);
        var cur;
        try { cur = JSON.parse(blob); }
        catch (e) { return [["JSON", "PARSE"], "json::exists: " + e.message]; }
        for (var i = 0; i < path.length; i++) {
            if (cur === null || typeof cur !== "object") return "0";
            if (!(path[i] in cur)) return "0";
            cur = cur[path[i]];
        }
        return "1";
    });
}

proc ::wacl::json::get {blob args} {
    ::wacl::js::call __wacl_json_get $blob {*}$args
}

proc ::wacl::json::exists {blob args} {
    ::wacl::js::call __wacl_json_exists $blob {*}$args
}

package provide wacl::json 1.0
