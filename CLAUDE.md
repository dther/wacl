# CLAUDE.md

Orientation notes for future Claude sessions on SurfTcl. Skim this before
making changes — a couple of these gotchas burned a session.

## What this is

SurfTcl is a **Tcl 9** distribution compiled to WebAssembly via Emscripten.
Originally written by Eckhard Lehmann (`ecky-l/wacl`) against Tcl 8.6,
revived by dther after ~9 years of bitrot and now cut over to Tcl 9.0.3.
The 8.6 archaeology demo is preserved at tag `v0.0.1-minimal`; master is
Tcl 9 only. The README has the revival story and the running to-do.

The current live demo is `wacl-minimal-demo/index.html`: a Tcl REPL in
the browser, ~200 lines, one file, no framework, no build step on the
page side.

The project was renamed from **Wacl** to **SurfTcl** — marking the
graduation from "Eckhard's 8.6 demo revived" to a modern Tcl 9 web
distribution, and making the fork clear (BSD-3 clause 3, respecting the
upstream). The rename is **contents-only so far**: identifiers,
namespaces, and package names are now `surftcl` / `SurfTcl` / `SURFTCL`,
but **file and directory names still use the old `wacl-*` form** (e.g.
`opt/wacl.c`, `opt/waclNotifier.c`, `wacl-minimal.{js,wasm}`, the
`wacl-minimal-demo/` dir, `packages/wacl-dom/`, `tests/wacl-*.test`, the
`tcl/wacl` AMD module id, the `wacl.wasm` symlink). Those file renames,
and the GitHub repo (`dther/wacl`), are a later pass. Upstream
attribution to Eckhard Lehmann's original `ecky-l/wacl` stays as-is.

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

## Build map

The Makefile uses real file-target dependencies (`tcl`, `tcl/unix/Makefile`,
`tcl/unix/libtcl9.0.a` as targets, not phony names) so `make` does the
right thing for incremental rebuilds — touching only `opt/wacl.c` and
running `make` skips the libtcl build entirely. A cold `make fullclean
&& make` takes about two minutes end-to-end (includes the Tcl tarball
download); subsequent edits to surftcl source are about ten seconds.
`distclean` deliberately preserves the tarball so iteration on the
Tcl-source-tree level doesn't keep re-downloading; `fullclean` removes
the tarball too if you want a genuinely cold cache.

- `make` (default) = `make minimal`: build `wacl-minimal.{js,wasm}` and
  copy into `wacl-minimal-demo/`. This is the **Asyncify** baseline
  (`-DSURFTCL_ASYNCIFY -sASYNCIFY`, ~4MB) — it's what makes `::surftcl::js::yield`
  and `interp.EvalAsync` work; see `docs/event-loop.md`. JSPI will shed
  most of the size tax once it's cross-browser.
- `make tcl`: download Tcl 9.0.3 source tarball and unpack to `tcl/`.
- Build requires Emscripten 5.0.2 specifically — newer versions break
  this build chain. emsdk lives at `/opt/emsdk`; source
  `/opt/emsdk/emsdk_env.sh` before each session's first build.
- `make tcl/unix/Makefile` runs `emconfigure` then sed-patches Tcl's generated
  `Makefile` to:
    1. Add `${ZLIB_INCLUDE}` to `CC_SWITCHES`. Tcl 9 upstream bug — when
       configure falls back to internal `compat/zlib/`, generic .c files
       (`tclEvent.c`, `tclZlib.c`) can't find `zlib.h` because
       `CC_SWITCHES` doesn't include the bundled-zlib path. Per-rule
       Z*.o builds get it but TCL_OBJS don't. Worth upstreaming.
    2. `-DTCL_THREADS=0`. Tcl 9 removed `--disable-threads`; the
       preprocessor symbol defaults to 1 in `tclInt.h`. Threads pull in
       `pthread_kill`, which Emscripten doesn't provide because
       wasm/WebWorkers have no POSIX signals. See README/TIP arc on
       making this configure-detectable upstream eventually.
- `opt/wacl.c`, `opt/waclNotifier.c`, and `opt/waclAppInit.c` are
  compiled separately and linked at the final emcc step — not bundled
  into `libtcl9.0.a` via patch the way ecky-l's 8.6 build did. Simpler
  and means we don't patch the Tcl source tree at all.
- Build flags include `ALLOW_TABLE_GROWTH=1` so the JS bridge can use
  `Module.addFunction`, and `EXPORTED_RUNTIME_METHODS` carries
  `cwrap`, `FS`, `addFunction`, `removeFunction`, `getValue`,
  `UTF8ToString`. The Tcl-callable C entry points exported via
  `EXPORTED_FUNCTIONS` are `_main`, `_SurfTcl_GetInterp`, `_SurfTcl_Eval`,
  `_SurfTcl_GetStringResult`, the JS-bridge four (`_SurfTcl_RegisterJsFn`,
  `_SurfTcl_RevokeJsFn`, `_SurfTcl_SetJsResultString`,
  `_SurfTcl_AppendJsErrorCodeElement`), and `_SurfTcl_ServiceEvents` (the
  JS-driven event pump; see `docs/event-loop.md`). If you add a C entry
  point and call it from JS, both lists need updating.
- JS glue around the emcc output lives in `js/`:
  - **`js/preJsRequire.js`** — the AMD wrapper prepended to the emcc
    output. **This is the configurability lever for the JS↔Tcl bridge.**
    Default to changing things here rather than working around them at
    the page level.
  - `js/postJsRequire.js` — closes the AMD factory and returns the
    `{ onReady }` object. Appended via `--post-js`.
  - `js/preJs.js`, `js/postJs.js` — variants for non-AMD builds;
    unused by `make minimal`.
- Build artifacts in `wacl-minimal-demo/`:
  - `wacl-minimal.js` and `wacl-minimal.wasm` — regenerated by
    `make minimal`. Source edits in `js/preJsRequire.js` won't appear
    in the .js until rebuild.
  - `wacl.wasm` is a symlink → `wacl-minimal.wasm` because the AMD
    shim resolves `tcl/` to `""` and the AMD wrapper hardcodes the
    wasm filename as `"wacl.wasm"`.

## How the Tcl library gets into the browser

Tcl 9 ships its standard library as `libtcl9.0.3.zip` (the build
produces it next to `libtcl9.0.a`). On a native build, `TclZipfs_AppHook`
mounts a zip appended to the executable or shared library; in wasm
we have neither, so the cutover does it explicitly:

1. `make minimal` passes `--embed-file tcl/unix/libtcl9.0.3.zip@/lib/tcl.zip`
   to emcc, baking the zip into the wasm virtual FS at `/lib/tcl.zip`.
2. `opt/waclAppInit.c::main` calls `TclZipfs_Mount(NULL, "/lib/tcl.zip",
   "//zipfs:/lib/tcl", NULL)` and sets `::tcl_library` to
   `//zipfs:/lib/tcl/tcl_library` before `Tcl_Init`.
3. `Tcl_Init`'s search script finds `init.tcl` at the canonical mount
   point. `clock format`, `package require Tcl`, etc. all work.

This same mechanism is the lever for the broader app-packaging story
the user has gestured at: one `app.zip` of Tcl scripts and data that
works the same on `wish`, Windows tclkit, and SurfTcl-in-browser, with
extensions baked into the runtime per-platform. The JS bridge in
`preJsRequire.js` doesn't yet expose `TclZipfs_Mount` to JS — when it
does, page-level developers will be able to `surftcl.mountZip(buf, mp)`
to load an application zip at runtime.

## The Emscripten stdout gotcha — important

Emscripten's runtime captures `out = Module.print` **exactly once**,
during `run()`, after the async wasm fetch resolves. After that
capture, mutating `Module.print` is a no-op.

The original `_Result.stdout` setter (`set stdout(fn) { Module.print = fn }`)
no longer works against current Emscripten — by the time `onReady` fires,
`out` has already been cached, so reassigning `Module.print` has no
effect on `puts`.

Per dther, this setter *did* work in ecky-l's original demo, ~9 years ago.
Best guesses for what changed: Emscripten's runtime initialization
semantics shifted across four major versions (very likely), or the
original output path went through tdom somehow (equally plausible, no
direct proof). Worth keeping in mind if you go digging — there's a
specific commit somewhere in Emscripten that introduced the
capture-once behavior.

**Current fix (commit 3f04879):** stdout/stderr are wired through
`FS.init(stdin, stdout, stderr)` in `preRun`, the idiomatic Emscripten
path. The callbacks are byte-granular (`null` flushes); we line-buffer
into text and hand the decoded string to mutable `_stdoutSink` /
`_stderrSink` slots. The `set stdout` / `set stderr` accessors on
`_Result` swap the slot. As a bonus the same wiring gives us a real
stdin channel: `_Result.pushStdin(text)` appends bytes for a Tcl
`gets stdin` to drain; `closeStdin()` signals EOF. The earlier
`Module.print`-wrapper trick is gone.

## The Module-shadowing trap

`preJsRequire.js` contains:

```js
var Module;
if (typeof Module === 'undefined')
  Module = eval('(function() { try { return Module || {} } catch(e) { return {} } })()');
```

The intent looks like "use a pre-set global Module if available, else
make a fresh one." It doesn't work. The `var Module;` inside the factory
shadows the outer Module; the eval IIFE scope-walks and finds the local
undefined Module before reaching anything global. Setting `window.Module`
before loading `wacl-minimal.js` has no effect on the factory's Module.

The factory then does `delete window.Module` at the very end, which is a
clue that the original author *thought* this worked.

If you need to inject anything Module-shaped from outside, do it
through `preJsRequire.js` directly, not by trying to pre-set a global.

## The JS bridge (`::surftcl::js` + `interp.js`)

How the inner Tcl interp reaches back into the page. The pre-rework
`::surftcl::jscall fcnPtr returnType argType ?arg?` was a type-dispatch
tarpit — Cartesian-product macros over one return type and one arg
type — and exposed raw Emscripten function-table indices to every Tcl
caller. The rework (commit 5d60999) replaces it with a name-keyed
registry the host fills explicitly.

**Surface:**

  JS host side:
    interp.js.register(name, fn)   // fn(args:string[]) -> value
    interp.js.revoke(name)
    interp.js.names()              // array of registered names

  Tcl side:
    ::surftcl::js::call NAME ?ARG ...?   // varargs; JS sees a string[]
    ::surftcl::js::names                 // Tcl list of registered names
    ::surftcl::js::revoke NAME           // voluntarily decline a grant

There is no Tcl-side `register`. The host grants what the inner
interp may call; the inner interp can introspect, invoke, and
voluntarily relinquish — but never *expand* — the registry.
`revoke` is the Tcl-side seal: a bootstrap script does its
`package require`s, wires whatever surface it wants, then revokes
`eval` (and anything else broad) before user input lands. A polite
guest can decline what it was offered; only the host can offer.

**Argument convention.** All args after NAME are passed varargs-style
and arrive on the JS side as one array of strings. To pass an existing
Tcl list as args, expand with `{*}`:

    ::surftcl::js::call myFn {*}$argList

The C bridge is type-blind; all marshalling and any application-level
type checking lives in the registered JS function. Anything richer
than a string (objects, arrays of non-strings) is the caller's job to
serialize — JSON is the default since the ecosystem already speaks it.

(History note: the initial rework took a single ARG_LIST argument and
ran `Tcl_ListObjGetElements` over it. That looks tidier, but every JS
code block passed through `::surftcl::js::call eval { ... }` had its `})`
patterns trip Tcl's list-syntax parser. Varargs sidestep that entirely
since `{...}` is just one brace-group at parse time, not a list.)

**Return-value protocol** (normalized in `_makeJsShim` before the
C side hears about it):

    undefined / null     -> TCL_OK, result ""
    any scalar           -> TCL_OK, result = String(value)
    [status, value]:
      number n             -> Tcl return code n. 0=OK, 1=ERROR,
                              2=RETURN, 3=BREAK, 4=CONTINUE;
                              >=5 are custom catch'able codes.
      "ok" | "error" | "return" | "break" | "continue"
                           -> the corresponding code by name (lowercase).
      any other string     -> TCL_ERROR, ::errorCode = {string}
      array of strings     -> TCL_ERROR, ::errorCode = that list
                              (suitable for `try ... trap PATTERN`)
    thrown Error           -> TCL_ERROR with the message

**Bootstrap-then-seal pattern.** The intended use of the registry
is: page registers `eval` (and any other broad capabilities) at
startup, Tcl bootstrap uses `::surftcl::js::call eval { ... }` to
build a domain-specific surface (DOM ops, fetch, WebSocket, file
pickers), then the page calls `interp.js.revoke("eval")` before
any untrusted script gets to evaluate. The capability is gone for
real — `revoke` removes the hash entry and frees the Emscripten
function-table slot via `Module.removeFunction`.

This is not a rare flow. It is the **default shape** for SurfTcl
apps. Documented as such so we don't drift toward "expose
everything by default" out of laziness.

**Implementation map.** Side-channel `SurfTcl_SetJsResultString` /
`SurfTcl_AppendJsErrorCodeElement` (called by the JS shim before its
return) carry value and errorCode-list across the wasm boundary.
The function's own integer return is the Tcl status code. C side
in `opt/wacl.c`; JS side in `js/preJsRequire.js`.

## Failure surface: `surftcl.supportURL` and `surftcl.onError`

The floor is honesty, not an implicit white lie.

`surftcl.onError(context, error)` is the bridge's default error sink,
invoked by the packages (and by anything else inside surftcl that fails
in a way the page should know about). The default implementation
writes to stderr (visible in the terminal if wired, console.error
otherwise) and fires an `alert()`. The alert text branches on whether
`surftcl.supportURL` is set:

  - Set: "A fatal surftcl error has occurred. Please report it via:
    {supportURL} — Details: {context+message}"
  - Not set: "A fatal surftcl error has occurred but the developer has
    not named a point of contact through surftcl.supportURL. Details:
    {context+message}"

The unset branch is deliberate self-incrimination. Generic "please
report this to the developer" is a white lie — it implies a path
forward when there might not be one. By naming the omission, the
default forces the developer into one of two right things:

  1. Set `surftcl.supportURL` to any contact pointer (URL, mailto:,
     GitHub issues link, "tweet at @us") — strings, not validated.
     The alert then names where to go.
  2. Override `surftcl.onError = function (context, error) { ... }` —
     route errors anywhere (Sentry, an in-app toast, /dev/null).
     Overriding IS the formal, in-code acceptance of responsibility.

**Upstream-support stance.** Projects that ship the "developer has
not named a point of contact" alert are unsupported by upstream
until they do one of the two above. Both are easy. The default is
calibrated so that the *only* way to be invisible is to actively
take responsibility for being so.

"Fatal" is the right descriptor even for recoverable one-shots: surftcl
doesn't know whether a failed handler put the application in a bad
state, can't make that claim, so falls back on the developer — who,
by not setting supportURL, has abdicated naming the recovery path.
As far as the end user is concerned, that's fatal.

The `surftcl::dom` listener catch, the `surftcl::chan` postevent/onData
catches, and the wasm-instantiation failure all route through
`surftcl.onError` (or, for wasm instantiation, an inline version of
the same shape — postRun hasn't fired yet at that point).

## Re-entrant Tcl_Eval — fence REMOVED

There used to be a fence here: `SurfTcl_Eval` refused any call made while
another `SurfTcl_Eval` was on the stack. It's **gone** (commit "Remove
requirement for re-entrant execution"), because it was solving the wrong
problem. Re-entrant evaluation is normal and safe in Tcl — the engine
re-enters itself for every `[bracket]` substitution, `eval`, and
`fileevent` callback. JS and Tcl now share one thread and one event
loop, so a JS callback invoked mid-Tcl (via `::surftcl::js::call`) calling
straight back into `SurfTcl_Eval` is exactly the cooperation we want.

We deliberately do **not** save/restore interpreter state around the
nested call: a JS-side failure that propagates leaves its
errorInfo/errorCode intact, so the Tcl side can `catch` it or let it
bubble to the failure surface. Silent isolation — papering over a nested
error to keep frames "clean" — is the one thing we reject. Runaway
self-recursion is caught by Tcl's own nesting limit ("too many nested
evaluations (infinite loop?)"), not by us. `tests/wacl-bridge.test`
locks this in: re-entrancy works, a thrown JS exception is a catchable
Tcl error, and a nested-`Eval` failure surfaces with its trace.

The one place re-entrancy *does* get dangerous is across a **yield**
(suspending an evaluation while JS runs and re-enters Tcl) — but that
needs a guard, not a fence, and the yield primitive doesn't exist yet.
See `docs/event-loop.md` for the full analysis.

## The wacl-* packages (`/packages/`)

Three Tcl packages that demonstrate the bootstrap-then-seal pattern in
miniature. Each one self-installs its JS shims at `package require` time
using the host-granted `eval`, then exposes a Tcl-side surface. The
intended flow is `require X; require Y; …; ::surftcl::js::revoke eval` —
after the revoke, no more bridged packages can be added, but the ones
already loaded keep working.

  - **surftcl::json** — `surftcl::json get $blob ?key…?`,
    `surftcl::json extract $blob ?key…?`, and
    `surftcl::json exists $blob ?key…?`. Path traversal in JS via
    `JSON.parse`. `get` returns coerced Tcl values (objects and arrays
    re-stringified as JSON so you can recurse); `extract` returns the
    raw JSON fragment (strings stay quoted, `true` stays a bare
    boolean, `null` stays bare), mirroring rl_json's read-side
    disambiguator. Use `extract` when you need to distinguish the
    JSON string `"true"` from the boolean `true` — they collapse to
    the same Tcl representation via `get`. Errors are catchable via
    `try ... trap {JSON PARSE}` or `{JSON BAD_PATH}`. Deliberately no
    `stringify`: Tcl can't discriminate the string `"true"` from
    boolean `true`, so Tcl→JSON is ambiguous in a way JSON→Tcl
    isn't. Same precedent will apply when typed-write commands
    (`json string`, `json bool`, …) get added.

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
    `prop`, `call`, `event` all accept JS-style dot-paths
    (`target.id`, `classList.add`, `dataset.userId`, …). `style`
    accepts CSS-hyphenated or camelCase via getProperty/setProperty.
    `html` and `append` are not sanitised — same XSS surface as
    setting innerHTML directly; use `text` for untrusted data.
    `each` is driven from Tcl-side so each step is its own Tcl_Eval
    frame on the JS side — the eval-fence stays out of the way, and
    `break`/`continue`/`return` from the body work normally.
    Nested `each` is supported; the currentElement is stacked.
    Composes via `addEventListener` (no interference with other
    listeners). The eval-fence does **not** trip on real DOM events:
    they fire from the JS event loop between Tcl_Eval calls, not
    synchronously inside one.

  - **surftcl::chan** — `set ch [surftcl::chan open NAME]` returns a Tcl
    reflected channel (`chan create`), bidirectional and binary.
    JS side attaches with `globalThis.surftclChan.attach(NAME)` to
    get `{onData, write, close}`. Bytes round-trip via latin-1 to
    preserve identity (a Uint8Array byte N becomes JS code-point N
    becomes Tcl code-point N becomes the literal byte N out of a
    binary-translation channel). This is the interface for
    **application special-use channels** — event queues, pseudo-signal
    callbacks, WebSocket wrappers — the lever for "JS as a Tcl event
    queue": channels with `fileevent` are how arbitrary JS-side events
    (clicks, fetch responses, WebSocket frames) dispatch into Tcl with
    all the normal event-loop machinery. It is **not** the mechanism for
    stdio — stdin/stdout/stderr are Tcl's own standard channels (Tcl
    names and initialises them, over `FS.init` fd devices; see the
    Emscripten stdout gotcha). Don't route the standard streams through
    `surftcl::chan`. (Reflected channels are pure Tcl + the JS bridge —
    **no Emscripten pipes**; the only Emscripten dependency in the whole
    event story is Asyncify, for the yield.) JS writes currently defer
    `chan postevent` via `setTimeout(0)`; that dodge existed only for the
    removed re-entrancy fence and can become a synchronous post now that
    re-entry during a suspension is proven safe (a next-step in
    `docs/event-loop.md`).

**Bootstrap dependency.** Package shims look up the surftcl handle as
`globalThis.surftcl`, blessed in `preJsRequire.js`'s postRun via
`Object.defineProperty(globalThis, "surftcl", { value: _Result,
writable: false, configurable: false })`. The binding is locked
against accidental shadowing (a stray `var surftcl = ...` at page scope
would otherwise clobber it silently — JS gives no warning); the
properties on the object stay mutable so `surftcl.stdout = fn` etc.
still work. `__interp` and `__surftcl` remain on `window` as console
aliases for shorter typing, but they're no longer load-bearing.

**Layout.** `/packages/wacl-<name>/{pkgIndex.tcl, wacl-<name>.tcl}` —
first-party source at the repo root (moved out of the demo so the demo
can consume them as released artifacts rather than carry the source).
One main `.tcl` per package, code-as-documentation, no minification.
The demo pages still fetch the loose `.tcl` files at boot
(`Module.FS.writeFile` + `lappend auto_path /packages`) for development;
once the JS-side `TclZipfs_Mount` cwrap lands, the demo will mount the
released zips instead.

## The `/ext` package pipeline

`ext/Makefile` turns each `/packages/wacl-<name>/` into a release zip
`ext/build/wacl-<name>.zip` (gitignored). The zip holds the package
directory at its root — `wacl-json/pkgIndex.tcl`, … — so a consumer
mounts it anywhere and `lappend auto_path <mountpoint>` finds it in the
`wacl-<name>/` subdirectory, exactly like an on-disk auto_path entry:

    tcl::zipfs::mount /path/to/wacl-dom.zip //zipfs:/pkg/wacl-dom
    lappend auto_path //zipfs:/pkg/wacl-dom
    package require surftcl::dom

`make packages` (or `make -C ext`) builds them; `make test` builds then
runs the headless suite. The headless runner **loads packages from
these zips, not from the loose source** — so every CI run validates the
actual shipped artifact, not just the source tree. (The browser runner
still uses the loose-file fetch path; it'll move to released zips with
the JS mount bridge.)

The same machinery will slice vendored tcllib modules into
`tcllib-<module>.zip` artifacts on tcllib's own module boundaries (each
subdir already ships a `pkgIndex.tcl`); not wired yet.

**Release channel.** `.github/workflows/release-packages.yml` publishes
the built zips to a single, continuously-overwritten pre-release tagged
`unstable-bleeding`, giving stable download URLs
(`…/releases/download/unstable-bleeding/wacl-dom.zip`) whose contents
change without notice. The tag name, the pre-release flag, and the notes
all shout "testing only." Stable, immutable `vX.Y` releases are a
separate later workflow, added when a real downstream needs stabilising;
the demo will default to those and keep the bleeding edge in a clearly
marked corner. Triggers: `workflow_dispatch` + push to `master` (add a
dev branch under `push: branches:` to publish the edge from there).

## Test harness (`tests/` + `wacl-minimal-demo/tests/` + CI)

`tcltest`, the test framework that ships in Tcl core, drives every
suite. No new payload — it's already in `/lib/tcl.zip` and
`package require tcltest` finds it through `auto_path`. Files at
`tests/*.test` use the standard `-body / -result / -returnCodes /
-errorCode / -match` machinery, which is exactly the shape we need
for asserting the structured error codes the packages raise.

Three runners, all sourcing the same test files against the same
runtime:

  - **`tests/all.tcl`** — driver. Sources every `*.test` in lexical
    order in *one* interpreter (NOT `tcltest::runAllTests`, which
    spawns a child interp per test file and loses the JS-side `eval`
    grant we just bootstrapped). Prints a manual summary at the end.

  - **`tests/run-headless.mjs`** — CLI runner. Loads the wasm in a
    node `vm` context, mounts the `ext/build/wacl-*.zip` package
    artifacts via zipfs (so the suite runs against the real shipped
    zips — build them first with `make -C ext`, or use `make test`),
    injects the test files, sources `all.tcl`, exits non-zero on any
    failure. Used by CI; locally it's faster than reloading the browser
    page.

  - **`wacl-minimal-demo/tests/index.html`** — browser runner. Live
    log streams tcltest's output as it executes, classified into
    pass / fail / skip / header by line prefix and coloured. Status
    pill in the header reports counts and overall outcome. Same
    page-relative fetch pattern as the playground but reaching one
    level higher for `tests/` since they're at repo root rather than
    under `wacl-minimal-demo/`.

Two implementation choices worth knowing before extending the suite:

  - Individual `.test` files deliberately don't call `cleanupTests`.
    The default behaviour prints a per-file summary *and resets*
    `::tcltest::numTests`, which would zero out the totals before the
    runner reads them. `all.tcl` prints the unified summary at the
    end and leaves the counts intact.
  - Tests that assert `-returnCodes error` also need a result spec
    (either exact, `-match glob -result {prefix:*}`, or
    `-match glob -result *`). tcltest's default `-result` is `""` and
    it compares against the actual error message, so without an
    explicit result spec every error-throwing test fails with a
    confusing "Result was: ..., should have been (exact matching): "
    mismatch. The `-match glob -result {json::get:*}` style adds an
    honest assertion that the message points at the right command.

**CI.** `.github/workflows/tests.yml` builds the package zips
(`make -C ext`) then runs `node tests/run-headless.mjs` on every push,
every pull request, and on `workflow_dispatch`. No wasm rebuild — uses
the committed artifacts in `wacl-minimal-demo/`; the only build is the
cheap zip step the runner needs. `release-packages.yml` is the second
workflow (the `unstable-bleeding` channel, above). Workflows under
`.github/workflows/` normally require the GitHub `workflow` scope on the
pushing token, which the in-session automation tokens don't always
carry; when they lack it, landing these files needs a `git push` from a
developer credential or a paste through the GH web UI. End-to-end test
runtime ~7 seconds.

Four suites exist now: `wacl-json.test`, `wacl-chan.test`,
`wacl-dom.test`, and `wacl-bridge.test` (the `::surftcl::js::*` surface
itself — re-entrancy + error propagation, the regression guard for the
removed eval-fence).

Two constraint conventions came out of writing the chan/dom suites, and
new suites should reuse them rather than reinvent:

  - **`dom` constraint.** surftcl::dom needs a real `document`, which the
    headless node runner doesn't have. `wacl-dom.test` sets
    `testConstraint dom` from `typeof document !== "undefined"` and tags
    every test `-constraints dom`. Result: the *same file* runs for real
    in the browser runner and skips cleanly headless. A fake DOM in the
    headless runner was rejected as testing the fake, not the package;
    real DOM-in-CI (jsdom) stays a deliberate, separate infra decision.

  - **`eventLoop` constraint — RETIRED.** It used to gate tests that
    needed an event to actually *fire*, deliberately red as honest
    markers of the unsolved event semantics. The event-loop work this
    described has since landed (custom notifier + `SurfTcl_ServiceEvents`
    pump + eval-fence removal — see `docs/event-loop.md`), so the
    markers are gone: `chan-fileevent-1.1` now tests real dispatch (JS
    feeds bytes → `chan postevent` → `update` pumps → the `fileevent`
    fires and drains) and passes; `dom-bind-3.1` passes for synchronous
    dispatch now that the fence is gone. No test carries `eventLoop` any
    more. The one thing still beyond a synchronous test — a *real*
    event-loop-deferred DOM dispatch — waits on the yield construct
    (Asyncify `js::yield`), documented in `docs/event-loop.md`, not
    faked as a failing test.

  - **The chan byte contract is tested, NUL included.** `wacl-chan.test`
    asserts bytes 1..255 round-trip with full fidelity *and* that NUL
    truncates at the bridge (the js::call return crosses as a C string).
    The truncation is a tested fact, not a latent surprise — see the
    `chan-j2t-2.2` case.

## Packaging philosophy: zipfs as the lever, no package manager

Tcl 9's zipfs gives us first-class app packaging for free: a single
`app.zip` of scripts mounted via `TclZipfs_Mount` works the same on
`wish`, a Windows tclkit, and SurfTcl-in-browser. This is the basis
for the planned LOVE-clone-style packaging story and worth treating
as a first-class concern.

**Decision: SurfTcl does not run a package manager.** The primitive
is `surftcl::mount <buffer-or-url> <mountpoint>` and that's it. No
`package unknown` hook that fetches transparently, no registry, no
resolver, no lock files. The user (or their bootstrap Tcl) names
the zips they want, mounts them, and adds the mountpoint to
`auto_path`. Costs are visible because the user typed them.

Rationale (per dther): "if you don't use it, you shouldn't pay for
it, and a package manager hides costs." Many tcllib modules
degrade gracefully when optional deps are missing; a transitive
resolver would pull in dependencies the user doesn't actually need.
The maintenance hazard ("no one wants to say no") is real and
worth avoiding.

**Distribution shape.** Pre-built per-module tcllib zips are
release artifacts on GitHub (slicing on tcllib's existing module
boundaries — each subdirectory already has its own `pkgIndex.tcl`,
so zero patching). The release page IS the package index: a static
document, not a service. Catalog as markdown is documentation, not
infrastructure. Browser cache makes repeated loads effectively
free. No CDN to run.

**App-bundle shape.** A SurfTcl app's build step is roughly
`zip app.zip my-scripts/ tcllib-http/ tcllib-json/ ...`. The page
mounts that one zip at boot; no runtime resolution, ever. This is
the LOVE-clone-style "one bundle, runs everywhere" model.

The `TclZipfs_Mount` wrapper that exposes this from JS still needs
writing — see the punted list.

## Demo pages (`wacl-minimal-demo/`)

Three pages, each self-contained, no framework, AMD shim only.

- **`/`** (the REPL) — the original Tcl terminal in the browser.
- **`/playground/`** — DOM-from-Tcl playground. A 4×4 grid of cards
  + a task list as the sandbox, side-by-side with a Tcl editor and
  output panel, plus an examples ribbon along the bottom that loads
  and runs pre-written scripts (light odds, rainbow hues, spin all,
  dim non-primes, click-to-toggle, insert-all-four, etc.). Styling
  leans on modern CSS — Grid for layout, oklch for perceptually
  uniform colour cycling per-card via `--hue` custom properties,
  cubic-bezier transitions, keyframe animations.
- **`/tests/`** — browser test runner. Live-log output coloured by
  outcome, status pill that reports counts at the end.

### Detail: the REPL (`index.html`)

- One file, inline CSS + JS, no framework.
- 5-line AMD shim (`define`, `require`, `require.toUrl`) avoids pulling
  in RequireJS.
- `SurfTclTerminal({ mount, prompt, onLine })` is the channel abstraction.
  Methods: `write`, `writeErr`, `clear`, `setPrompt`, `focus`.
  Transport-agnostic by design — the same shape works for a remote
  tclsh over WebSocket with no API change.
- Input is a `<textarea>` that auto-grows. Enter submits, Shift+Enter
  inserts a newline. Up/Down navigate history *only when the textarea
  is empty* — otherwise arrows move the cursor as you'd expect in a
  chat box.
- The ANSI parser handles SGR only (`\x1b[...m`): colors 30–37 / 40–47,
  bold, reset. Other escape sequences are consumed silently rather than
  rendered as garbage.
- The demo also registers `alert` and `eval` as example JS bridge
  entries on the inner interp, so `::surftcl::js::call eval {Math.PI}`
  works out of the box. These are *examples* — in a real app the page
  would register exactly what it wants the inner interp to reach, then
  `revoke("eval")` before any untrusted code lands.
- `window.__interp` and `window.__surftcl` are exposed for JS-console
  debugging: try `__interp.js.register("ping", a => "pong:" + a.join(","))`
  then `__interp.Eval("::surftcl::js::call ping {one two three}")`.

## Things explicitly punted

- **`TclZipfs_Mount` from JS.** The runtime supports it; the JS bridge
  doesn't expose it yet. Wire it via `cwrap` and add a JS-side
  convenience that takes an `ArrayBuffer` / fetches a URL. This is the
  primitive the packaging philosophy above depends on; bring it up
  alongside the first real multi-zip demo.
- **Per-module tcllib release zips.** Prebuilt `tcllib-<module>.zip`
  artifacts published on GitHub releases, sliced on tcllib's existing
  module boundaries. Markdown catalog in the repo lists what's in each
  zip and its loose deps. No CDN, no resolver — the release page IS
  the index.
- **`tdom` against Tcl 9.** Next big feature. After looking at how
  ecky-l's wacl used tdom, the patch turns out to be lighter than
  feared: SurfTcl's old DOM bridge is string-in/string-out — tdom never
  sees the actual DOM, it just parses HTML strings the page hands it
  via the JS bridge. So the patch is mostly linting and command
  renaming for Tcl 9 compatibility; the `::surftcl::RootDOM` namespace
  variable / `WithRoot` helper this doc previously gestured at is
  largely moot — the *caller* (in surftcl) is what holds the document
  reference, not tdom. The legacy `::surftcl::dom` command in `opt/wacl.c`
  still exists as a stop-gap; once tdom lands, equivalents go via
  registered JS functions (`querySelectorAll`, etc.) and `::surftcl::dom`
  retires.
- **Replace the sed-patches with named patch files.** The two
  `surftclconfig` sed lines (`ZLIB_INCLUDE`, `-DTCL_THREADS=0`) should
  become files in a `patches/` directory (quilt-style) or a Tcl script
  that does the rewrites. Deferred until the build pipeline is
  otherwise stable.
- **Real-time stdin.** The REPL is JS-driven: Enter → `interp.EvalAsync(line)`.
  `_Result.pushStdin` provides a one-shot queue, but `gets stdin` doesn't
  yet block-and-yield. This lives in the **standard-channel layer** —
  stdin is Tcl's own channel over the `FS.init` fd device, **not** a
  `surftcl::chan`. The Asyncify yield makes the real fix possible: a `gets
  stdin` on an empty device can `::surftcl::js::yield`, JS feeds via
  `pushStdin` during the suspension, and the read resumes — keeping the
  page live while it waits, with no Emscripten pipe. Implement it in that
  device/standard-channel layer; do not reach for `surftcl::chan`.
- **Runaway-loop weak preemption (cooperative-model backstop).** A Tcl
  loop that never yields — `while 1 {puts lol}` — freezes the whole
  browser tab: the single thread is held, so the JS event loop can't
  turn. This is the inherent cost of cooperative scheduling (the reason
  preemptive scheduling exists). A JS-side watchdog CANNOT catch it — any
  `setTimeout`/`setInterval` is itself frozen by the loop it would watch.
  But Tcl can preempt *itself*, C-side: `interp limit` / `Tcl_LimitSetTime`
  set a wall-clock deadline that `Tcl_LimitCheck` **polls** at command-
  granularity checkpoints during execution (tclInterp.c:3441 — `Tcl_GetTime`
  compared to the deadline, no event loop, no timer), raising a catchable
  `TCL LIMIT TIME` error that unwinds the loop and hands control back to
  JS. So the tab **recovers** — the loop is genuinely stopped, not just
  explained after the fact. (Granularity caveat: it fires at Tcl command
  boundaries, so a pure-C tight loop with no command dispatch wouldn't
  checkpoint — out of scope.)

  Two complementary fixes, undecided:
    1. **Hard time limit** as the backstop (with the friendly error naming
       `supportURL`). Catches output-*less* loops (`while 1 {}`). Threshold
       tension: browsers throw their own "page unresponsive" dialog at
       ~10–15s, so a 30–60s limit lets the browser win the race; a shorter
       5–10s limit recovers gracefully but could kill a legit long
       no-yield computation.
    2. **Implicit yield on stdout/stderr write** — unbuffered interactive-
       shell semantics: an output-producing loop yields as it writes and
       stays live. Doesn't catch output-less loops.
  Likely both: a time-*throttled* yield on the output path (yield only if
  >X ms since the last — cheap + responsive, the "ioctl" rediscovered)
  for graceful work, plus the hard limit as the safety net. Pre-release
  must-address; see `docs/event-loop.md`.
- **Spitballed idea worth recording.** The user has floated: instead of
  emulating raw mode, expose a JS keypress event stream to Tcl
  (key-downs *and* key-ups), with Tcl scheduling events when bytes
  arrive. Considered strictly better than readline-style integration
  for browser use cases — no byte-interpretation overhead, more data
  (key-up signals), and it costs basically nothing on modern hardware.
  Not implemented. Do not implement without an explicit ask.
- **A WebSocket transport** so the same terminal can front a remote
  tclsh. `SurfTclTerminal`'s shape was designed for it.
- **C extensions via wasm side modules.** Possible in principle —
  Emscripten supports `MAIN_MODULE`/`SIDE_MODULE`, and Tcl's stubs
  table is exactly the right shape — but the build-time ABI matching
  between main and side modules means it's not "drop any .so into the
  zip and go." Note for the LOVE-clone packaging story: "C extensions
  need to be compiled for surftcl specifically, same way desktop
  extensions are compiled against a specific Tcl version."
  Inter-extension ABI: use `Tcl_PkgProvideEx` / `Tcl_PkgRequireEx`
  with a version word at offset 0 of a vtable struct — this is the
  Tcl-orthodox version of the "extensions expose a C ABI to each
  other through a stubs-table" pattern, mature, per-interp, cleaned
  up at interp teardown. `Tcl_GetAssocData` is the side door for
  more dynamic patterns.
- **Event-loop integration (primary architecture target) — core LANDED.**
  See `docs/event-loop.md` for the full design and current state. The JS
  and Tcl event loops are integrated on the **main thread** via a custom
  non-blocking notifier (`Tcl_SetNotifier`, no Tcl-source patch) that JS
  pumps with `SurfTcl_ServiceEvents` (`Tcl_DoOneEvent(TCL_DONT_WAIT)`); the
  yield primitive (`::surftcl::js::yield` → `SurfTcl_Yield` → `emscripten_sleep`
  under Asyncify, one-suspension guard) lets a Tcl computation relinquish
  to the JS loop and resume in place, with `interp.EvalAsync` the
  Promise-returning top-level entry and the pure-Tcl `update` wrapper the
  idiom. Spike + API checks pass. Main-thread-first because that's where
  sound / WebGL / gamepad input (the SDL3 surface, what games need) live —
  a DOM-less worker can't reach them, so Tk is the floor, not the ceiling.
  Worker mode is the secondary, clean-separation path. Still ahead:
  real-time stdio/FIFO channels, async DOM listeners, JSPI when it's
  cross-browser.
- **Notifier / `TCL_THREADS=0` — corrected.** Earlier notes here claimed
  Tcl 9's notifier "uses `pthread_kill` to wake the notifier thread."
  Verified false against 9.0.3: the notifier thread is woken by a
  self-pipe (`triggerPipe`); `pthread_kill` appears only in
  `TclAsyncNotifier`, the async-signal re-routing path. `TCL_THREADS=0`
  is needed for a **link-time** reason (that signal path references
  `pthread_kill`, absent in our non-`-pthread` Emscripten build), not a
  runtime wakeup dependency. The custom-notifier route above sidesteps
  the whole platform notifier anyway. A self-pipe-fallback TIP is still
  worth filing upstream for "threads but no signals" targets.
- **Finish the SurfTcl rename (file/dir names + repo).** The
  *identifier* rename is done (see "What this is"): C symbols
  (`SurfTcl_*`), namespace (`::surftcl::*`), package names
  (`surftcl::dom` etc.), JS handle (`globalThis.surftcl`), build define
  (`SURFTCL_ASYNCIFY`). Still on the old `wacl-*` form: file and
  directory names (`opt/wacl.c`, `wacl-minimal-demo/`, `packages/wacl-dom/`,
  `tests/wacl-*.test`, output `wacl-minimal.{js,wasm}` + the `wacl.wasm`
  symlink), the `tcl/wacl` AMD module id, and the GitHub repo
  `dther/wacl`. Rename those together (each cascades into path
  references — Makefile, runners, demo fetches, ext zip globs) and
  rebuild; do not do it piecemeal. Keep the `ecky-l/wacl` attribution.

## Conventions

- Work happens on the `claude/<adjective-name>` branch the session
  instructions specify. Never push to master.
- Commit → push after each working unit; `~/.claude/stop-hook-git-check.sh`
  enforces this.
- Do NOT create a PR unless explicitly asked.
- Style: descriptive names; comments only where the WHY is non-obvious;
  no TODO annotations; no "added for X" cross-references.

## Out-of-scope directories

- `wacl-minimal-demo/wacl/` — the old jQuery + RequireJS demo. Reference
  only; do not extend it. The top-level `wacl-minimal-demo/index.html`
  replaced it.
- `ecky-l.github.io/` — submodule for the old project webpage. Ignore.
