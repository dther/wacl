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

# TODO(dther) The JS for this is included in surftcl-bootstrap.mjs.
# I'd like it all to be over there since JSON is useful enough to always
# include, but I haven't figured out an interface to run init scripts.

if {[info commands ::surftcl::js::names] eq ""} {
    error "surftcl::json requires the surftcl JS bridge"
}

namespace eval ::surftcl::json {
    namespace export get extract exists
    namespace ensemble create

    proc get {blob args} {
        ::surftcl::js::call surftcl_json_get $blob {*}$args
    }
    
    proc extract {blob args} {
        ::surftcl::js::call surftcl_json_extract $blob {*}$args
    }
    
    proc exists {blob args} {
        ::surftcl::js::call surftcl_json_exists $blob {*}$args
    }
}

package provide surftcl::json 0.0
