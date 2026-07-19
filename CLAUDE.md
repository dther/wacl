# CLAUDE.md

Orientation notes for future Claude sessions on SurfTcl. Skim this before
making changes — a couple of these gotchas burned a session.

## What this is

SurfTcl is a **Tcl 9** distribution compiled to WebAssembly via
Emscripten and shipped as an **ES6 module**. Originally written by
Eckhard Lehmann (`ecky-l/wacl`) against Tcl 8.6, revived by dther after
~9 years of bitrot, cut over to Tcl 9.0.3, and since stripped back hard.
The 8.6 archaeology demo is preserved at tag `v0.0.1-minimal`. The
README has the revival story.

The current live demo is `wacl-minimal-demo/index.html`: a Tcl REPL in
the browser, one file, no framework, no build step on the page side. It
is also the reference example for how a page is expected to consume the
ES6 module.

**Current architectural stance (mid-2026 refactor):** the browser owns
and pumps the event loop; Tcl is a cooperative guest of it. The Asyncify
build, `::surftcl::js::yield`, `interp.EvalAsync`, and
`SurfTcl_ServiceEvents` are all **removed** — Asyncify's complexity (and
a silent-stack-clobber failure mode) wasn't worth it for the current
goal: a small Tcl interpreter with a simple API that experienced Tcl
developers can evaluate. `emscripten_set_main_loop` pumps
`Tcl_DoOneEvent(TCL_DONT_WAIT)` instead. The Tcl event loop itself is
considered sufficient for asynchrony for now; the idiom for
blocking-feel control flow is coroutines scheduled as events, with maybe
a Promise-shaped API later. A JSPI-driven event loop is a post-alpha
prototype, deliberately not designed yet — no API lock-in until real
developers have tried SurfTcl. See `docs/event-loop.md` for the current
mechanics and the Asyncify retrospective.

Rename status: identifiers, namespaces, and package names are `surftcl`
/ `SurfTcl` / `SURFTCL`, and the runtime source files are renamed too:
`opt/surftcl.{c,h}`, `opt/surftclAppInit.c`, `opt/surftclNotifier.c`,
`js/surftcl-bootstrap.mjs`, build outputs `surftcl.mjs` +
`surftcl.wasm`. Still on the old `wacl-*` form: the `wacl-minimal-demo/`
dir, `packages/wacl-*/`, `tests/wacl-*.test`, the ext zip names
(`wacl-<name>.zip`), and the GitHub repo `dther/wacl`. Those rename
together (each cascades into path references — Makefile, runners, demo
fetches, ext zip globs); do not do it piecemeal. Upstream attribution to
Eckhard Lehmann's original `ecky-l/wacl` stays as-is.

## Aesthetic constraints

The user has a specific position on what this project should look like:

- The web is feature-complete; raw HTML/CSS/JS is preferred over frameworks.
- Each file should read as a self-contained example. Single-file demos
  over bundles. Code-as-documentation over comments.
- Minification belongs in build artifacts only; never in user-facing
  files. Opaque bundlers (Webpack, RequireJS-via-config, etc.) are
  treated as the disease, not the cure.
- Comments only where the WHY is non-obvious. No "added for X" notes,
  no rotting cross-references.
- The terminal is **not** a VT100 emulator. Teletype emulation in 2026
  is treated as a mistake we're choosing not to repeat. Only SGR (color)
  is implemented from the ANSI repertoire; other escape sequences are
  stripped silently.
- ES6 modules are how SurfTcl is distributed. Don't add things to the JS
  glue until absolutely necessary.

## Build map

The Makefile uses real file-target dependencies (`tcl`,
`tcl/unix/Makefile`, `tcl/unix/libtcl9.0.a`, the `.o` files,
`surftcl.mjs`) so `make` does the right thing for incremental rebuilds —
touching only `opt/surftcl.c` and running `make` skips the libtcl build
entirely. A cold `make fullclean && make` takes about two minutes
end-to-end (includes the Tcl tarball download); subsequent edits to
surftcl source are about ten seconds. `distclean` deliberately preserves
the tarball so iteration on the Tcl-source-tree level doesn't keep
re-downloading; `fullclean` removes the tarball too.

- `make` (default) = `make minimal`: emcc links `surftcl.o` +
  `surftclAppInit.o` + `surftclNotifier.o` + `libtcl9.0.a` into
  **`surftcl.mjs` + `surftcl.wasm`** at the repo root, then copies them
  and `js/surftcl-bootstrap.mjs` into `wacl-minimal-demo/`. No Asyncify
  — the Asyncify recipes are gone (commit "Remove build recipes that
  require Asyncify").
- ES6 module output via `-s MODULARIZE -s EXPORT_ES6 -s
  EXPORT_NAME=createSurfTcl`. The emcc glue resolves `surftcl.wasm`
  relative to `import.meta.url`, so the wasm just needs to sit next to
  `surftcl.mjs` — the old `wacl.wasm` symlink and AMD `require.toUrl`
  games are gone.
- `make tcl`: download Tcl 9.0.3 source tarball and unpack to `tcl/`.
- Build requires Emscripten 5.0.2, pending a version bump. Later
  versions are currently untested, but we want to target the latest
  stable version. emsdk location varies by container (`/opt/emsdk`
  historically; check `which emcc` / look for `emsdk_env.sh`). If it's
  absent, clone `emscripten-core/emsdk`, `./emsdk install 5.0.2 &&
  ./emsdk activate 5.0.2` (~a minute on a good pipe). Source
  `emsdk_env.sh` before each session's first build.
- `make tcl/unix/Makefile` runs `emconfigure` then sed-patches Tcl's
  generated `Makefile` to:
    1. Add `${ZLIB_INCLUDE}` to `CC_SWITCHES`. Tcl 9 upstream bug — when
       configure falls back to internal `compat/zlib/`, generic .c files
       (`tclEvent.c`, `tclZlib.c`) can't find `zlib.h` because
       `CC_SWITCHES` doesn't include the bundled-zlib path. Per-rule
       Z*.o builds get it but TCL_OBJS don't. Worth upstreaming.
    2. `-DTCL_THREADS=0`. Tcl 9 removed `--disable-threads`; the
       preprocessor symbol defaults to 1 in `tclInt.h`. Threads pull in
       `pthread_kill`, which Emscripten doesn't provide because
       wasm/WebWorkers have no POSIX signals. This is a link-time need,
       not a runtime one — see `docs/event-loop.md`.
- The `opt/` sources are compiled separately and linked at the final
  emcc step — not bundled into `libtcl9.0.a` via patch the way ecky-l's
  8.6 build did. We don't patch the Tcl source tree at all.
- Build flags include `ALLOW_TABLE_GROWTH=1` so the JS bridge can use
  `Module.addFunction`, and `EXPORTED_RUNTIME_METHODS` carries `cwrap`,
  `ccall`, `FS`, `addFunction`, `removeFunction`, `getValue`,
  `UTF8ToString`. The C entry points in `EXPORTED_FUNCTIONS` are
  `_main`, `_SurfTcl_GetInterp`, `_SurfTcl_Eval`,
  `_SurfTcl_GetStringResult`, and the JS-bridge four
  (`_SurfTcl_RegisterJsFn`, `_SurfTcl_RevokeJsFn`,
  `_SurfTcl_SetJsResultString`, `_SurfTcl_AppendJsErrorCodeElement`).
  If you add a C entry point and call it from JS, both lists need
  updating.
- JS glue is now **one file**: `js/surftcl-bootstrap.mjs`, the ES6
  module a page actually imports. It wraps the emcc output (it does
  `import createSurfTcl from "./surftcl.mjs"`, so the servable copy must
  sit next to the built `surftcl.mjs` — which is why `make minimal`
  copies both into the demo dir). **This is the configurability lever
  for the JS↔Tcl bridge**; default to changing things here rather than
  working around them at page level. The old `preJsRequire.js` /
  `postJsRequire.js` / `preJs.js` / `postJs.js` AMD-era files are
  deleted, and with them the Module-shadowing trap this doc used to
  warn about.
- Build artifacts (`surftcl.mjs`, `surftcl.wasm`, and the copies in
  `wacl-minimal-demo/`) are generated, not committed. The demo site
  carries its own copies in the surftcl-demo repo.

## Known stale / broken (as of the playground-fix pass)

The refactor outran some of the tree. Most of the earlier entries were
fixed on the playground-fix pass (playground page, REPL `update` wrapper
and console hints, `make surftcl-demo`/`clean`/`.gitignore`,
`tests/run-headless.mjs` rewritten against the ES6 module,
`tests/run-async.mjs` deleted). Prune entries as they get fixed:

- **`packages/wacl-chan` JS shims have broken `this` bindings**: the
  rework replaced the `surftcl` global with `this`, but inside the
  `setTimeout` callbacks and the registered plain `function`s `this` is
  not the Runtime (undefined in module strict mode), so the JS-side
  write→`chan postevent` path and the `onError` calls would throw. Tests
  stay green because `chan-fileevent-1.1` posts from the Tcl side.
  dther's FIXME at the top of the file already condemns the whole
  package to a heavy rework — treat it as scheduled for demolition, and
  don't build on it.
- **`Runtime.onError` is marked FIXME in the bootstrap** ("this error
  surface doesn't work right, yet"); `TclPanic` is a stub (its
  constructor references an undefined variable and the C side never
  throws it).

## How the Tcl library gets into the browser

Tcl 9 ships its standard library as `libtcl9.0.3.zip` (the build
produces it next to `libtcl9.0.a`). On a native build, `TclZipfs_AppHook`
mounts a zip appended to the executable or shared library; in wasm
we have neither, so we do it explicitly:

1. `make minimal` passes `--embed-file tcl/unix/libtcl9.0.3.zip@/lib/tcl.zip`
   to emcc, baking the zip into the wasm virtual FS at `/lib/tcl.zip`.
2. `opt/surftclAppInit.c::main` calls `TclZipfs_Mount(NULL, "/lib/tcl.zip",
   "//zipfs:/lib/tcl", NULL)` and sets `::tcl_library` to
   `//zipfs:/lib/tcl/tcl_library` before `Tcl_Init`.
3. `Tcl_Init`'s search script finds `init.tcl` at the canonical mount
   point. `clock format`, `package require Tcl`, etc. all work.

This same mechanism is the lever for the broader app-packaging story:
one `app.zip` of Tcl scripts and data that works the same on `wish`,
Windows tclkit, and SurfTcl-in-browser. A recorded intent in
`surftclAppInit.c`: after all initialisation, load `main.tcl` from the
zipfs if present, mirroring what `zipfs mkimg` gives an appended-zip
tclsh — so "give SurfTcl a zip that would work on a desktop" becomes the
idiomatic packaging path. The JS bridge doesn't expose `TclZipfs_Mount`
yet — see the punted list.

## The ES6 module (`js/surftcl-bootstrap.mjs`)

How a page gets an interpreter (see the REPL for the canonical usage):

    import("./surftcl-bootstrap.mjs").then((surftcl) => {
      const interp = surftcl.default;   // the Runtime object
      interp.stdout.sink = (text) => ...;
      const result = interp.Eval("expr {2+2}");
    });

The module top-level-awaits `createSurfTcl(Module)`, so by the time the
import resolves the wasm is instantiated, `main()` has run (mounted the
library zip, created the interp, started the main loop), and the default
export — the **Runtime** object, assembled in `Module.postRun` — is
live. Named exports: `Module`, `Runtime`, `TclException`, `TclPanic`,
`TclResult`, `JsFunctionRegistry`, `VERSION`.

Runtime surface (the public JS API):

  - `Eval(script)` — synchronous, the thinnest sensible wrapper around
    `Tcl_EvalEx`. Returns the result string; on a non-OK code it fetches
    `::errorInfo`/`::errorCode` and throws a `TclException` carrying
    `errorCode`, `errorMessage`, `errorInfo`. There is no async Eval any
    more.
  - `stdin` / `stdout` / `stderr` — the stdio objects (below).
  - `js` — the `JsFunctionRegistry` (below), plus `GrantEval()`,
    `RevokeEval()`, `RevokeEvalPermanently()` on the Runtime itself.
  - `supportURL` / `onError(context, error)` — the failure surface
    (below; currently FIXME'd).
  - `Module` — the Emscripten module, for `FS` access etc.
  - Underscore-prefixed cwraps (`_eval`, `_getInterp`, …) are internal.

There is **no global handle any more**: the old
`Object.defineProperty(globalThis, "surftcl", …)` blessing is gone, and
pages that want console conveniences set their own (the demos assign
`window.__interp` / `window.__surftcl`). The one global-ish backref is
`Module.SurfTcl = Runtime`, set in postRun so the **wasm side** can
reach the registry from `EM_ASM` (the C `::surftcl::js::revoke` uses it).

`TclException` / `TclPanic` / `TclResult` use `Symbol.for`-keyed brands
with `Symbol.hasInstance`, so `instanceof` works across module-instance
boundaries.

## Stdio

stdin/stdout/stderr are wired through `FS.init(stdin, stdout, stderr)`
in `Module.preRun` — the idiomatic Emscripten path, and the only one
that works (Emscripten's runtime captures `out = Module.print` exactly
once during `run()`; mutating `Module.print` afterwards is a no-op —
that gotcha burned the original revival and is why the setter approach
of the ecky-l era can't come back).

The wiring is three module-level objects, exposed as `Runtime.stdin` /
`.stdout` / `.stderr`:

  - `stdout` / `stderr`: byte-granular callbacks line-buffer into text
    and hand the decoded string to a mutable **`.sink`** slot
    (`interp.stdout.sink = (text) => term.write(text)`). Default sinks
    log to the JS console.
  - `stdin`: `interp.stdin.write(text)` appends bytes to a queue that
    the FS device drains one byte per read; `interp.stdin.close()`
    forbids further writes (queued bytes still drain). An **empty queue
    reads as EOF immediately** — the device callback returns null.

Known limitation, recorded as `DEFER(channel rework)` in the source:
Emscripten's stdio devices break Tcl's assumptions — they are never
blocking, and there's no way to convey "no data yet" as distinct from
EOF, so `chan eof` misreports on stdin and `chan event readable` won't
behave. The fix lives in this standard-channel/device layer (possibly
replacing the devices with ones that report EAGAIN), **not** in
`surftcl::chan`; don't route the standard streams through reflected
channels.

## The JS bridge (`::surftcl::js` + `JsFunctionRegistry`)

How the inner Tcl interp reaches back into the page. C side in
`opt/surftcl.c`; JS side is the `JsFunctionRegistry` object in
`js/surftcl-bootstrap.mjs`.

**Surface:**

  JS host side (`interp.js`):
    register(name, fn)   // fn(args: string[]) -> value | TclResult
    revoke(name)         // returns 1 if it was registered JS-side
    names()              // iterator over registered names (Map keys)

  Tcl side:
    ::surftcl::js::call NAME ?ARG ...?   // varargs; JS sees a string[]
    ::surftcl::js::names                 // Tcl list of registered names
    ::surftcl::js::revoke NAME           // voluntarily decline a grant

There is no Tcl-side `register`. The host grants what the inner interp
may call; the inner interp can introspect, invoke, and voluntarily
relinquish — but never *expand* — the registry. Only the host can offer.

The registry is double-entry: the C side keeps a `name → function-table
index` hash (what `::surftcl::js::call` dispatches through); the JS side
keeps a `name → table pointer` Map (so `removeFunction` can free the
Emscripten table slot). The two are kept in sync in **both revoke
directions**: JS `revoke` calls the C `SurfTcl_RevokeJsFn` before
freeing its slot, and the Tcl `::surftcl::js::revoke` command `EM_ASM`s
into `Module.SurfTcl.js.revoke(...)` so the JS side cleans up too.
`register` over an existing name revokes the old entry first.

**Argument convention.** All args after NAME are passed varargs-style
and arrive on the JS side as one array of strings. To pass an existing
Tcl list as args, expand with `{*}`:

    ::surftcl::js::call myFn {*}$argList

The C bridge is type-blind; all marshalling and any application-level
type checking lives in the registered JS function. Anything richer than
a string (objects, arrays of non-strings) is the caller's job to
serialize — JSON is the default since the ecosystem already speaks it.

(History note: an earlier design took a single ARG_LIST argument and ran
`Tcl_ListObjGetElements` over it. Every JS code block passed through
`::surftcl::js::call eval { ... }` had its `})` patterns trip Tcl's
list-syntax parser. Varargs sidestep that entirely since `{...}` is one
brace-group at parse time, not a list.)

**Return-value protocol** (normalized in `JsFunctionRegistry.call`
before the C side hears about it). The old `[status, value]` array
protocol is **gone**; the structured form is now the `TclResult` class:

    undefined / null       -> TCL_OK, result ""
    any other value        -> TCL_OK, result String(value); if String()
                              itself throws, TCL_ERROR with errorCode
                              {SURFTCL JS BADTYPE <toString tag>}
    a TclResult            -> its code / value / options.errorCode used
                              directly (returned *or thrown* — a thrown
                              TclResult is caught and honored, the
                              "return this immediately" signal, which
                              makes sense for error/break)
    thrown Error/primitive -> TCL_ERROR, message from String(e),
                              errorCode {SURFTCL JS THREW <e.name>}

`TclResult` (exported class): `new TclResult(code, value, options)` with
code a non-negative integer or one of `"ok" | "error" | "return" |
"break" | "continue"` (TCL_OK…TCL_CONTINUE; integers ≥5 are custom
catchable codes), plus static conveniences `TclResult.ok(v)`,
`.error(v, {errorCode: [...]})`, `.return/.break/.continue`. For error
results with no explicit errorCode the default is `NONE`, mirroring
Tcl's `error`. Value/errorCode cross the wasm boundary via the
side-channel pair `SurfTcl_SetJsResultString` /
`SurfTcl_AppendJsErrorCodeElement`, called by the shim before it
returns; the function's own integer return is the Tcl status code.
Unknown NAME raises errorCode `{SURFTCL JS NOTFOUND}` from the C side.

**`GrantEval()` and the eval scoping.** `interp.GrantEval()` registers
the canonical `eval` capability. It uses *direct* `eval`, deliberately:
inside the evaluated code, `this` is the Runtime and the bootstrap
module's scope is visible (so `TclResult`, the stdio objects, etc. are
in reach — the package shims depend on this). A legacy `surftcl`
binding to the Runtime is also in scope (slated for removal; `this` is
canonical). The escape hatch for global-scope evaluation is
`(0, eval)(...)`. The shim stringifies objects with `JSON.stringify`
before returning. `RevokeEval()` revokes it; `RevokeEvalPermanently()`
also replaces `GrantEval` with a thrower so it can't come back.

**Bootstrap-then-seal pattern.** The intended use of the registry is:
page calls `GrantEval()` (and registers any other broad capabilities) at
startup, Tcl bootstrap uses `::surftcl::js::call eval { ... }` to build
a domain-specific surface (DOM ops, fetch, WebSocket, file pickers),
then the page revokes `eval` before any untrusted script gets to
evaluate. The capability is gone for real — revoke removes both hash
entries and frees the function-table slot. This is not a rare flow; it
is the **default shape** for SurfTcl apps. Two recorded design intents
build on it (DEFER comments in the bootstrap): a Tcl-side
`surftcl::pledge` command (OpenBSD-style — pledge the packages you'll
use, which sources them then seals eval permanently; pledging `eval`
itself is an error), and making the registry **per-interpreter** in
keeping with "interpreters share nothing by default."

## Failure surface: `surftcl.supportURL` and `surftcl.onError`

The floor is honesty, not an implicit white lie. **Currently FIXME'd in
the bootstrap — the wiring doesn't work right yet — but the design
stands:**

`Runtime.onError(context, error)` is the default error sink for
failures the page should know about. The default implementation writes
to stderr's sink and fires an `alert()` whose text branches on whether
`Runtime.supportURL` is set:

  - Set: "A fatal surftcl error has occurred. Please report it via:
    {supportURL} — Details: {context+message}"
  - Not set: "A fatal surftcl error has occurred but the developer has
    not named a point of contact through surftcl.supportURL. Details:
    {context+message}"

The unset branch is deliberate self-incrimination. Generic "please
report this to the developer" implies a path forward when there might
not be one. By naming the omission, the default forces the developer
into one of two right things: set `supportURL` to any contact pointer
(strings, not validated), or override `onError` — overriding IS the
formal, in-code acceptance of responsibility. Projects that ship the
"developer has not named a point of contact" alert are unsupported by
upstream until they do one of the two. "Fatal" is the right descriptor
even for recoverable one-shots: surftcl can't claim to know whether a
failed handler left the app in a bad state, so it falls back on the
developer, who by omission has abdicated naming the recovery path.

## Re-entrant Tcl_Eval — no fence

There used to be a fence: `SurfTcl_Eval` refused any call made while
another `SurfTcl_Eval` was on the stack. It's long gone. Re-entrant
evaluation is normal and safe in Tcl — the engine re-enters itself for
every `[bracket]` substitution, `eval`, and `fileevent` callback. JS and
Tcl share one thread and one event loop, so a JS callback invoked
mid-Tcl (via `::surftcl::js::call`) calling straight back into
`SurfTcl_Eval` is exactly the cooperation we want.

We deliberately do **not** save/restore interpreter state around the
nested call: a JS-side failure that propagates leaves its
errorInfo/errorCode intact, so the Tcl side can `catch` it or let it
bubble to the failure surface. Silent isolation — papering over a nested
error to keep frames "clean" — is the one thing we reject. Runaway
self-recursion is caught by Tcl's own nesting limit ("too many nested
evaluations (infinite loop?)"), not by us. `tests/wacl-bridge.test`
locks this in. `SurfTcl_Eval` forces `TCL_EVAL_GLOBAL`: a JS-initiated
evaluation is a fresh top-level call from outside, so it runs at global
scope regardless of what Tcl frame happens to be current.

## The wacl-* packages (`/packages/`)

Three Tcl packages (all `package provide … 0.0`) that demonstrate the
bootstrap-then-seal pattern in miniature. Each self-installs its JS
shims at `package require` time using the host-granted `eval`, then
exposes a Tcl-side surface. The intended flow is `require X; require Y;
…; ::surftcl::js::revoke eval` — after the revoke, no more bridged
packages can be added, but the ones already loaded keep working. Shim
errors are raised with `TclResult.error(msg, {errorCode: [...]})`.

  - **surftcl::json** — `surftcl::json get $blob ?key…?`,
    `surftcl::json extract $blob ?key…?`, and
    `surftcl::json exists $blob ?key…?`. Path traversal in JS via
    `JSON.parse`. `get` returns coerced Tcl values (objects and arrays
    re-stringified as JSON so you can recurse); `extract` returns the
    raw JSON fragment (strings stay quoted, `true` stays a bare
    boolean, `null` stays bare), mirroring rl_json's read-side
    disambiguator. Use `extract` when you need to distinguish the JSON
    string `"true"` from the boolean `true`. Errors are catchable via
    `try ... trap {JSON PARSE}` or `{JSON BAD_PATH}`. Deliberately no
    `stringify`: Tcl can't discriminate the string `"true"` from
    boolean `true`, so Tcl→JSON is ambiguous in a way JSON→Tcl isn't.
    Same precedent will apply when typed-write commands (`json string`,
    `json bool`, …) get added.

  - **surftcl::dom** — events plus DOM manipulation, consistently "trust
    JS to be JS." Selectors are standard CSS resolved through
    `document.querySelector`; the special selector `:this` refers to
    the currently-iterated element inside `surftcl::dom each`.
    Surface:
        surftcl::dom bind SEL EVENT SCRIPT       -> handle
        surftcl::dom unbind HANDLE
        surftcl::dom event FIELD                 -> string
        surftcl::dom prop  SEL PATH ?VALUE?      -> string | void
        surftcl::dom style SEL CSSPROP ?VALUE?   -> string | void
        surftcl::dom call  SEL METHOD ?ARG ...?  -> string
        surftcl::dom html  SEL ?HTML?            -> string | void
        surftcl::dom text  SEL ?TEXT?            -> string | void
        surftcl::dom before SEL HTML             -> void
        surftcl::dom prepend SEL HTML            -> void
        surftcl::dom append SEL HTML             -> void
        surftcl::dom after  SEL HTML             -> void
        surftcl::dom remove SEL
        surftcl::dom each   SEL BODY             -> count
    `prop`, `call`, `event` all accept JS-style dot-paths (`target.id`,
    `classList.add`, `dataset.userId`, …). `style` accepts
    CSS-hyphenated or camelCase. `html` and the insertion subcommands
    are not sanitised — same XSS surface as innerHTML; use `text` for
    untrusted data. `each` is driven from the Tcl side so
    `break`/`continue`/`return` from the body work normally; nested
    `each` stacks the currentElement. Composes via `addEventListener`.

  - **surftcl::chan** — `set ch [surftcl::chan open NAME]` returns a Tcl
    reflected channel (`chan create`), bidirectional and binary. JS
    attaches with `globalThis.surftclChan.attach(NAME)` to get
    `{onData, write, close}`. Bytes round-trip via latin-1 to preserve
    identity. This is the interface for **application special-use
    channels** — event queues, WebSocket wrappers, "JS as a Tcl event
    queue": channels with `fileevent` are how JS-side events dispatch
    into Tcl with the normal event-loop machinery. It is **not** the
    mechanism for stdio (see Stdio above). **Slated for heavy rework**
    — dther's FIXME at the top objects to the size of the JS-side FIFO
    reimplementation (much of it scar tissue from the removed
    re-entrancy fence: the `setTimeout(0)` postevent deferrals are no
    longer needed), and the rework may move parts into C. It also has
    broken `this` bindings on the JS-write path (see Known stale /
    broken). Don't extend it; rework it.

**Layout.** `/packages/wacl-<name>/{pkgIndex.tcl, wacl-<name>.tcl}` —
first-party source at the repo root. One main `.tcl` per package,
code-as-documentation, no minification. The demo pages fetch the loose
`.tcl` files at boot (`Module.FS.writeFile` + `lappend auto_path
/packages`) for development; once the JS-side `TclZipfs_Mount` cwrap
lands, the demo will mount the released zips instead.

## The `/ext` package pipeline

`ext/Makefile` turns each `/packages/wacl-<name>/` into a release zip
`ext/build/wacl-<name>.zip` (gitignored). The zip holds the package
directory at its root — `wacl-json/pkgIndex.tcl`, … — so a consumer
mounts it anywhere and `lappend auto_path <mountpoint>` finds it in the
`wacl-<name>/` subdirectory, exactly like an on-disk auto_path entry:

    tcl::zipfs::mount /path/to/wacl-dom.zip //zipfs:/pkg/wacl-dom
    lappend auto_path //zipfs:/pkg/wacl-dom
    package require surftcl::dom

`make packages` (or `make -C ext`) builds them. The same machinery will
slice vendored tcllib modules into `tcllib-<module>.zip` artifacts on
tcllib's own module boundaries (each subdir already ships a
`pkgIndex.tcl`); not wired yet.

**Release channel.** `.github/workflows/release-packages.yml` publishes
the built zips to a single, continuously-overwritten pre-release tagged
`unstable-bleeding`, giving stable download URLs
(`…/releases/download/unstable-bleeding/wacl-dom.zip`) whose contents
change without notice. The tag name, the pre-release flag, and the notes
all shout "testing only." Stable, immutable `vX.Y` releases are a
separate later workflow, added when a real downstream needs stabilising.
Triggers: `workflow_dispatch` + push to `master`. Workflows under
`.github/workflows/` normally require the GitHub `workflow` scope on the
pushing token, which the in-session automation tokens don't always
carry; landing or changing them may need a developer credential or the
GH web UI.

## Test harness (`tests/` + `wacl-minimal-demo/tests/`)

`tcltest`, the test framework that ships in Tcl core, drives every
suite. No new payload — it's already in `/lib/tcl.zip` and `package
require tcltest` finds it through `auto_path`. Files at `tests/*.test`
use the standard `-body / -result / -returnCodes / -errorCode / -match`
machinery, which is exactly the shape needed for asserting the
structured error codes the packages raise.

**The browser runner is authoritative** — dther tests against it.
Status of each:

  - **`tests/all.tcl`** — driver, still fine. Sources every `*.test` in
    lexical order in *one* interpreter (NOT `tcltest::runAllTests`,
    which spawns a child interp per test file and loses the JS-side
    `eval` grant). Prints a manual summary at the end.
  - **`wacl-minimal-demo/tests/index.html`** — browser runner,
    **works** (fixed for the ES6 module in commit "Make the browser
    testing page work again"). Live log streams tcltest's output,
    classified by line prefix and coloured; status pill reports counts.
  - **`tests/run-headless.mjs`** — CLI runner, **works** (rewritten
    against the ES6 module on the playground-fix pass: plain `await
    import` of `wacl-minimal-demo/surftcl-bootstrap.mjs` under node —
    no vm sandbox, no AMD shim). Loads the packages from the release
    zips `ext/` builds, so it validates the shipped artifact; `make
    test` builds both and runs it. The dom suite skips headless via
    the `dom` constraint, exactly as designed.
  - `tests/run-async.mjs` tested the removed Asyncify surface and was
    deleted on the same pass.

Four suites: `wacl-json.test`, `wacl-chan.test`, `wacl-dom.test`, and
`wacl-bridge.test` (the `::surftcl::js::*` surface itself — re-entrancy
+ error propagation, the regression guard for the removed eval-fence).
All remaining tests are synchronous; the Asyncify-era asynchrony tests
were removed with the feature. `chan-fileevent-1.1` survives because it
tests synchronous pumping — Tcl-side `chan postevent` + `update`
dispatching a fileevent — which needs no suspension.

Conventions to reuse when extending the suite:

  - Individual `.test` files deliberately don't call `cleanupTests`.
    The default behaviour prints a per-file summary *and resets*
    `::tcltest::numTests`, which would zero out the totals before the
    runner reads them. `all.tcl` prints the unified summary at the end
    and leaves the counts intact.
  - Tests that assert `-returnCodes error` also need a result spec
    (exact, `-match glob -result {prefix:*}`, or `-match glob -result
    *`). tcltest's default `-result` is `""` compared against the error
    message, so without a spec every error-throwing test fails with a
    confusing mismatch. The `-match glob -result {json::get:*}` style
    adds an honest assertion that the message points at the right
    command.
  - **`dom` constraint.** surftcl::dom needs a real `document`.
    `wacl-dom.test` sets `testConstraint dom` from `typeof document !==
    "undefined"` and tags every test `-constraints dom` — the same file
    runs for real in the browser and skips cleanly headless. A fake DOM
    headless was rejected as testing the fake, not the package.
  - **The chan byte contract is tested, NUL included.** Bytes 1..255
    round-trip with full fidelity *and* NUL truncates at the bridge
    (the js::call return crosses as a C string) — a tested fact, not a
    latent surprise (`chan-j2t-2.2`).

**CI.** There is no test CI. The old headless workflow ran against a
committed wasm that is no longer committed; it was removed rather than
left lying. Tests run manually before commits; the browser runner is
authoritative. CI returns once a build-binary release exists for it to
fetch, rather than rebuilding the wasm (~200MB emscripten tooling) every
run. `release-packages.yml` is unaffected — it only needs the zip step.

## Event loop: the browser owns it

Full mechanics and history in `docs/event-loop.md`; the short version:

- `opt/surftclNotifier.c` installs (via `Tcl_SetNotifier`, no Tcl-source
  patch) a notifier that **never blocks**: an indefinite wait returns -1
  ("would deadlock"), so blocking `Tcl_DoOneEvent` gives up at once and
  `vwait` on a JS-gated event raises `TCL EVENT NO_SOURCES` instead of
  hanging the tab. Fail fast and honestly.
- `opt/surftclAppInit.c` registers `SurfTclMainLoop` via
  `emscripten_set_main_loop(…, 0, 0)` — the browser calls it every
  requestAnimationFrame tick, and it runs one
  `Tcl_DoOneEvent(TCL_DONT_WAIT|TCL_ALL_EVENTS)`. (Recorded TODO: drain
  the queue per tick rather than one event.)
- So `after` timers and queued channel events fire "by themselves" from
  the page's point of view — no JS pump call needed. `update` still
  works as Tcl's own synchronous drain.
- The main-thread idiom is **callback / fileevent-driven**, mirroring
  JS itself. Blocking `vwait` is off the table on the main thread —
  there is no way to satisfy it without the JS loop, which it would be
  blocking. Coroutines scheduled as events are the intended
  blocking-feel idiom; a Promise-shaped "eval this asynchronously" API
  may wrap that later.

## Packaging philosophy: zipfs as the lever, no package manager

Tcl 9's zipfs gives us first-class app packaging for free: a single
`app.zip` of scripts mounted via `TclZipfs_Mount` works the same on
`wish`, a Windows tclkit, and SurfTcl-in-browser. This is the basis for
the planned LOVE-clone-style packaging story.

**Decision: SurfTcl does not run a package manager.** The primitive is
`surftcl::mount <buffer-or-url> <mountpoint>` and that's it. No `package
unknown` hook that fetches transparently, no registry, no resolver, no
lock files. The user (or their bootstrap Tcl) names the zips they want,
mounts them, and adds the mountpoint to `auto_path`. Costs are visible
because the user typed them. Rationale (per dther): "if you don't use
it, you shouldn't pay for it, and a package manager hides costs."

**Distribution shape.** Pre-built per-module tcllib zips as GitHub
release artifacts; the release page IS the package index — a static
document, not a service. Catalog as markdown is documentation, not
infrastructure. No CDN to run.

**App-bundle shape.** A SurfTcl app's build step is roughly `zip app.zip
my-scripts/ tcllib-http/ ...`; the page mounts that one zip at boot; no
runtime resolution, ever.

**The JS-code wrinkle** (recorded as `DEFER(better packaging)` in the
bootstrap): the blessed way to load JS code today is `eval` through the
granted bridge — genuinely fine for our trust surface, but locked out on
pages whose CSP forbids `unsafe-eval`. ES6 modules keep working there
but can't live inside a zipfs (they must be real files), which breaks
the "one zip for Web and Desktop" story. Decision: don't make the
inconvenient case the default; not a priority until users ask.

## Demo pages (`wacl-minimal-demo/`)

Three pages, each self-contained, no framework, loading the ES6 module
directly. The old jQuery/RequireJS-era demo (`wacl-minimal-demo/wacl/`)
is deleted. `make surftcl-demo` mirrors these into the separate
**surftcl-demo** repo (the GitHub Pages site, `DEMOREPO` pointing at
the checkout, sibling `../surftcl-demo` by default): the three pages,
the built `surftcl.mjs`/`surftcl.wasm`/`surftcl-bootstrap.mjs`, the
loose packages, and the test files the pages fetch at boot.

- **`/`** (the REPL) — the original Tcl terminal in the browser, and
  the reference for module usage: dynamic `import`, `surftcl.default`
  as the interp handle, `stdout.sink` wiring, `js.register("alert",…)`
  + `GrantEval()`, loose-package fetch into the wasm FS.
- **`/playground/`** — DOM-from-Tcl playground: 4×4 card grid + task
  list sandbox, Tcl editor, examples ribbon. Modern CSS (Grid, oklch
  per-card hues, keyframes). **Works** — migrated to the ES6 module on
  the playground-fix pass (dynamic `import`, `.sink` wiring, editor
  runs through the synchronous `Eval`). Feature-wise it's considered
  done for the alpha — the next capability step waits on an SDL canvas
  extension (see the punted list). The ribbon ends with the accepted
  "seal the bridge" example: `::surftcl::js::revoke eval`, then a
  failing `package require surftcl::json`, with the script's comment
  saying plainly that only the page's JS could re-grant, so within a
  page's lifetime the seal is final — refresh to reset.
- **`/tests/`** — browser test runner (see Test harness). Works.

### Detail: the REPL (`index.html`)

- One file, inline CSS + JS, no framework, no AMD shim any more.
- `SurfTclTerminal({ mount, prompt, onLine })` is the channel
  abstraction. Methods: `write`, `writeErr`, `clear`, `setPrompt`,
  `focus`. Transport-agnostic by design — the same shape works for a
  remote tclsh over WebSocket with no API change.
- Input is a `<textarea>` that auto-grows. Enter submits, Shift+Enter
  inserts a newline. Up/Down navigate history *only when the textarea
  is empty*.
- The ANSI parser handles SGR only (`\x1b[...m`): colors 30–37 / 40–47,
  bold, reset. Other escape sequences are consumed silently.
- Each submitted line goes through the synchronous `interp.Eval`;
  errors print `errorInfo` to the terminal's stderr styling.
- Registers `alert` and grants `eval` as example bridge entries. These
  are *examples* — a real app registers exactly what it wants, then
  revokes eval before untrusted code lands.

## Things explicitly punted

- **`TclZipfs_Mount` from JS.** The runtime supports it; the JS bridge
  doesn't expose it yet. Wire it via `cwrap` and add a JS-side
  convenience that takes an `ArrayBuffer` / fetches a URL. This is the
  primitive the packaging philosophy depends on; bring it up alongside
  the first real multi-zip demo.
- **`main.tcl` autoload from the app zip** (recorded in
  `surftclAppInit.c`): after all initialisation, source `main.tcl` if
  present in the zipfs, matching `zipfs mkimg` semantics on desktop.
- **Per-module tcllib release zips.** Prebuilt `tcllib-<module>.zip`
  artifacts on GitHub releases, sliced on tcllib's module boundaries.
  Markdown catalog in the repo. No CDN, no resolver.
- **`tdom` against Tcl 9.** After looking at how ecky-l's wacl used
  tdom, the patch is lighter than feared: the old DOM bridge is
  string-in/string-out — tdom never sees the actual DOM, it just parses
  HTML strings handed over the bridge. Mostly linting and command
  renaming for Tcl 9 compatibility. (The legacy `::surftcl::dom` C
  command this note used to reference has since been deleted from
  `opt/surftcl.c`; DOM equivalents go via registered JS functions.)
- **The JSPI event loop.** The whole suspend/resume tier — transparent
  yield, blocking-feel `gets stdin`, async DOM listeners — waits for
  JSPI to be baseline across browsers (est. another year), and gets
  prototyped only **after** the first public alpha announcement. The
  Asyncify findings that carry over are recorded in
  `docs/event-loop.md`.
- **`surftcl::chan` rework.** See the package section; also re-examine
  the Emscripten-stdio device layer (`DEFER(channel rework)`) in the
  same pass — EAGAIN-capable devices would fix the stdin EOF conflation.
- **`surftcl::pledge`** and a **per-interp function registry** — both
  recorded as DEFERs in the bootstrap; see the JS bridge section.
- **`TclPanic` wiring.** A `Tcl_Panic` handler on the C side that
  surfaces as the exported `TclPanic` error class instead of an
  Emscripten abort.
- **Replace the sed-patches with named patch files.** The two
  configure-time sed lines (`ZLIB_INCLUDE`, `-DTCL_THREADS=0`) should
  become files in a `patches/` directory (quilt-style) or a Tcl script.
  Deferred until the build pipeline is otherwise stable.
- **Real-time stdin.** The REPL is JS-driven: Enter → `interp.Eval(line)`.
  `interp.stdin.write` provides a one-shot queue, but a `gets stdin` on
  an empty queue reads EOF rather than waiting. The blocking-read story
  needs a suspension primitive (the JSPI tier) or EAGAIN-honest devices
  plus fileevent-driven reads; it lives in the standard-channel/device
  layer, not `surftcl::chan`.
- **Runaway-loop weak preemption (cooperative-model backstop).** A Tcl
  loop that never returns to the browser (`while 1 {puts lol}`) freezes
  the tab. A JS-side watchdog cannot catch it (its timer is frozen by
  the loop it would watch), but Tcl can preempt *itself*, C-side:
  `interp limit` / `Tcl_LimitSetTime` set a wall-clock deadline that
  `Tcl_LimitCheck` polls at command-granularity checkpoints, raising a
  catchable `TCL LIMIT TIME` error that unwinds the loop and hands
  control back to JS — the tab recovers. (Granularity caveat: a pure-C
  tight loop wouldn't checkpoint.) The complementary "implicit yield on
  stdout/stderr write" idea needed the yield primitive and is off the
  table until the JSPI tier. The hard time limit remains viable now and
  is a pre-release must-address; threshold tension: browsers throw
  their own "page unresponsive" dialog at ~10–15s.
- **Spitballed idea worth recording.** Instead of emulating raw mode,
  expose a JS keypress event stream to Tcl (key-downs *and* key-ups),
  with Tcl scheduling events when bytes arrive. Considered strictly
  better than readline-style integration for browser use. Not
  implemented. Do not implement without an explicit ask.
- **A WebSocket transport** so the same terminal can front a remote
  tclsh. `SurfTclTerminal`'s shape was designed for it.
- **An SDL canvas extension for Tcl.** The prerequisite for the games
  tier and the playground's next capability step. The extant SDL Tcl
  extensions don't fit: they're either complete X emulators (so Tk
  "just works") or SWIG-generated. It gets written fresh,
  **desktop-first** — proven as a normal Tcl extension on desktop, then
  compiled for SurfTcl like any C extension (next entry). Out of scope
  for the 0.1 alpha.
- **C extensions via wasm side modules.** Possible in principle —
  Emscripten supports `MAIN_MODULE`/`SIDE_MODULE`, and Tcl's stubs
  table is exactly the right shape — but build-time ABI matching means
  it's not "drop any .so into the zip and go." C extensions get
  compiled for surftcl specifically, same way desktop extensions are
  compiled against a specific Tcl version. Inter-extension ABI:
  `Tcl_PkgProvideEx` / `Tcl_PkgRequireEx` with a version word at offset
  0 of a vtable struct; `Tcl_GetAssocData` is the side door for more
  dynamic patterns.
- **Finish the SurfTcl rename (remaining paths + repo).** See "What
  this is" for what's left. Rename together, rebuild; don't do it
  piecemeal. Keep the `ecky-l/wacl` attribution.

## Conventions

- **Versioning.** The runtime is versioned the Tcl way (`a` = alpha,
  `b` = beta; see the `package` man page), starting at `0.1a1`.
  `SURFTCL_VERSION` in `opt/surftcl.h` is the single source of truth:
  `Tcl_PkgProvide` uses it directly, and the module's `VERSION` export
  asks the interp at boot (`package provide surftcl`) rather than
  keeping a copy that can drift. The packages version independently
  (tentatively semver, as is common for Tcl extensions); `0.0` means
  "not yet versioned."
- Work happens on the `claude/<adjective-name>` branch the session
  instructions specify. Never push to master.
- Commit → push after each working unit; `~/.claude/stop-hook-git-check.sh`
  enforces this.
- Do NOT create a PR unless explicitly asked.
- Style: descriptive names; comments only where the WHY is non-obvious;
  no TODO annotations; no "added for X" cross-references. (dther's own
  `TODO(dther)` / `FIXME(dther)` / `DEFER(topic)` markers are his to
  make; don't add Claude-authored ones.)
