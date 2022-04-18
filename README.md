# CLOSED BRANCH, DON'T MERGE - Notifier Rework

This branch was an attempt to rework the notifier to be based around a single suspend point in `Tcl_WaitForEvent`.
This allows theoretically infinite nested suspends.
The problem is that it also breaks the what I want to be the primary intended way of interacting with the interpreter,
which is through directly executing Tcl commands and receiving introspective answers.

In short: Asyncify works by stashing the C stack to a global whenever execution finishes,
and branching to the correct function appropriately the next time WASM code is executed.
This means that __any__ Tcl_Eval can potentially clobber Asyncify's stashed stack.
This was fine in earlier prototypes because the event loop was owned by the browser and pumped through `Tcl_DoOneEvent`,
and so the C stack was, in most cases, at a safe point for interpreter code to execute.

It is *theoretically* possible to stash the local variables ourselves,
and have something *resembling* an asynchronously threaded environment.
This is what Emscripten Fibres do.
But they only preserve the data from the stack, and not the return stack itself,
so it would likely break when trying to resume from a vwait, now that I think about it...

I've decided that this branch is outside of my expertise at the moment.
I'm retreating back to the working prototype and coming back when I have a better idea of Tcl's internals,
or when JSPI rolls around next year, and multiple C stacks become a native feature of mainline web browsers.

This rework won't work without some combination of the following:

- Reworking `Tcl_AsyncMark` to be the normal way to query results from the JS side, and re-model everything as promises.
  - i.e.: `Tcl_AsyncMark` maintains a queue every call,
   and points to a function with a queue of pending JS events, each resolving a waiting promise,
   and the Tcl interpreter enqueues them to be executed on the head of the event queue.
  - This sounds like the smartest way to do it.
- A virtual Tcl_Channel implementation which doesn't rely on evaluating `chan`
- A way of stashing the C stack, or having multiple in flight
  - Asyncify doesn't support this. The canonical way to do this is Emscripten Fibres.
  - JSPI would solve this problem cleanly, but it isn't available in Safari until later this year at the earliest.
