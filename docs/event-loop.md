# Event-loop integration — design direction

Status: **Provisional.** May be revised in future versions pending further testing.

A new discovery has been made: Because we don't need to stash multiple C stacks,
it's possible to simply "pause" on `Tcl_WaitForEvent` by awaiting a promise,
then "resume" by calling the resolve function. This means that Tcl can now safely own its event loop,
parallel to the JS side event loop, and they can co-operate on a shared reference to `resolve()`.
It's the same mechanism as `emscripten_sleep`, but with ways to resume outside of the fixed timeout.

**The Tcl side owns its own event loop.** It processes events until none remain,
then yields back to the browser using `Tcl_WaitForEvent`. It sets a JS timeout with a promise to resume itself.
The SurfTcl-specific implementations of these functions are defined in `surftclNotifier`.

In case it isn't clear,
**`Tcl_WaitForEvent` will always allow at least one full iteration of the JS event loop.**
This is to give the browser the opportunity to repaint the browser and enqueue user events.

**It is the browser's responsibility to initiate Tcl's event loop.**
This is done through calling the JS function, `SurfTcl.Start()`.
This JS function contains a long-running promise that will only normally resolve
when the interpreter exits.

The browser is allowed to override the normal behaviour through the exported JS API at any time.
The important calls are as follows:

- `SurfTcl.AlertNotifier()`: Resumes the Tcl event loop at the next microtask. Safe to call at any time.
  This maps directly to `Tcl_AlertNotifier`.
  - This works by resolving the promise created by `Tcl_WaitForEvent`.
- `SurfTcl.SetMaxBlockTime(ms)`: Sets the timeout **if it is smaller than the previous call to SetMaxBlockTime since Tcl_DoOneEvent or Tcl_ServiceAll was called.**
  This maps directly to `Tcl_SetMaxBlockTime`.
  - I don't know if Tcl requires I define the logic for this, or if it handles "check last given" on its own. I believe the latter because it's not part of the Tcl_Notifier API.

## Proposed (Not implemented)
- `SurfTcl.SetTickSpeed(ms)`: Sets the default maximum timeout before the Tcl event loop is guaranteed to be serviced next.
  `-1` causes it to be scheduled through `requestAnimationFrame`, the fastest reasonable default speed.
  **This does not map to any Tcl C function, and exists for JS convenience.**
  - If this is not given, the Tcl side won't call itself until the maximum possible timeout, which is `MAX_INT32`.
  - Alternatively: Call `AlertNotifier` clocked to a `requestAnimationFrame` callback?
    - Yes, microtasks execute **long before** the actual repaint. I've decided this is the preferred way to map Tcl event queue servicing to the `rAF.`
    - This is how the original prototype worked using `emscripten_set_main_loop`, which maps to `rAF` by default, I think? It's not important.
   I either left it default or mapped it to 60 frames per second, which is how the event loop was pumping.
- `SurfTcl.Exit()`: Resolves the promise created by `SurfTcl.Start()`. This is a normal exit.
- `SurfTcl.Panic(reason):` Rejects the promise created by `SurfTcl.Start()`, and emits the error given by `reason`.
- `SurfTcl.SetTimer(ms)`: Sets the timeout for when the Tcl event loop will be serviced next.
  This maps directly to `Tcl_SetTimer`.
  - This works through the JS function, `setTimeout`, with bookkeeping state variables inside the SurfTcl module.
  - **Don't do this one yet.** It's for when the Tcl loop is owned by the browser, and the loop *isn't* owned by the browser.
- `SurfTcl.GetTimer()`: Returns the last value that `SurfTcl.SetTimer(ms)` was given.
  **This does not map to the standard Tcl C API, and exists for JS convenience.**
  - Can't exist because SetTimer won't exist.

## RATIONALE
By owning the event loop entirely, Tcl code can safely nest `vwait` and `update` calls
without freezing the entire browser. On re-entry to the event loop, the Tcl code will hit
`Tcl_WaitForEvent`, yield to the browser (which may enqueue more events), and then
resume as a microtask once the timeout set by `Tcl_SetTimer` is reached.
