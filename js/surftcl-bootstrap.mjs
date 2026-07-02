// SurfTcl module entry point
// Performs the necessary setup for the SurfTcl Runtime,
// exporting the "Runtime" object by default, which contains
// SurfTcl's public JS API.

import createSurfTcl from "./surftcl.mjs"

// The Module object configures Emscripten glue code,
// and will be further populated at runtime.
//
// See the following docs for more info:
//   https://emscripten.org/docs/api_reference/module.html
export const Module = {
  noInitialRun: false,
  noExitRuntime: true,
};

// this gets populated with functions by Module['postRun'].
let Runtime = null;

// All Tcl Exceptions are this class
class TclException extends Error {
  constructor (errorCode, errorMessage, errorInfo) {
    let message = `SurfTcl.TclException: ${errorCode}\n${errorInfo || errorMessage}`
    super(message);

    // for catchers expecting a Tcl error
    this.errorCode = errorCode; // Tcl list of error codes, e.g., {POSIX NOENT}
    this.errorMessage = errorMessage;  // immediate message from the interp
    this.errorInfo = errorInfo || errorMessage;  // full ::errorInfo trace
  };
};

// DEFER(error handling) SurfTcl usage errors should probably get their own class, too.
//   As should "TclPanics", which interact with `Tcl_Panic` and unwind the interpreter.

// I/O ------------------------------------------------------------------------

// DEFER(channel rework) Emscripten's stdio devices break many of Tcl's assumptions.
//   They're never blocking, so `fconfigure` is false by default.
//   There also isn't a way to convey EOF on emscripten devices.
//   This isn't so bad for stderr and stdout, but causes issues with stdin,
//   which can't tell the difference between "no waiting data" and EOF.
//   `chan eof` will give false reports, and `chan event readable`
//   won't work as expected.

const Decoder = new TextDecoder();
const Encoder = new TextEncoder();

// Stdin queue. Runtime.pushStdin(text) appends; the FS.init input callback
// drains one byte at a time. Returning null from the callback means EOF —
// a script that does `gets stdin` with an empty queue gets EOF immediately.
const stdin = {
  queue: [],
  eof: false,

  write(text) {
    // this is to be used by external code.
    if (this.eof) throw Error("can't write to stdin after close");
    const bytes = Encoder.encode(text);
    for (const b of bytes) this.queue.push(b);
  },

  close() {
    // closing prevents future writes, but all enqueued bytes are guaranteed to go through
    this.eof = true
  },

  _read_callback() {
    // this is to be passed to the Emscripten module
    if (this.queue.length === 0) return null;
    return this.queue.shift()
  },
};

// I/O sinks. Page can override through `Runtime.stdin = (text) => {...}`.
// Output is delivered to FS.init per byte (or null for flush). We
// accumulate per stream until a newline or flush, then hand a decoded
// string to the sink.
const stdout = {
  buffer: [],
  flush() {
    if (this.buffer.length === 0) return;
    // FIXME(dther) is this seriously the right way to do it? **AND WHY DOESN'T IT EMIT AN ERROR BY DEFAULT??!**
    let text = Decoder.decode(new Uint8Array(this.buffer));
    this.buffer.length = 0;
    this.sink(text);
  },
  _write_callback(byte) {
    if (byte === null) this.flush();
    this.buffer.push(byte);
    // TODO(dther) we shouldn't assume line buffering
    if (byte == 10 /* \n */) this.flush();
  },
  sink: (text) => console.log('SurfTcl stdout: ' + text),
};

const stderr = {
  buffer: [],
  flush() {
    if (this.buffer.length === 0) return;
    let text = Decoder.decode(new Uint8Array(this.buffer));
    this.buffer.length = 0;
    this.sink(text);
  },
  _write_callback(byte) {
    if (byte === null) this.flush();
    this.buffer.push(byte);
    // TODO(dther) we shouldn't assume line buffering
    if (byte == 10 /* \n */) this.flush();
  },
  sink: (text) => console.log('SurfTcl stderr: ' + text),
};

// FS.init must be wired in preRun so /dev/stdin, /dev/stdout, /dev/stderr
// are devices backed by callbacks before main() runs and Tcl opens them.
// DEFER(channel rework) can we replace these entirely, so that they emit EAGAIN?
Module['preRun'] = function () {
  Module.FS.init(
    // bind is necessary because of how the "this" keyword works
    stdin._read_callback.bind(stdin),
    stdout._write_callback.bind(stdout),
    stderr._write_callback.bind(stderr)
  );
};

// ---- JS function registry ------------------------------------------------
//
// The host (page) grants JS functions to the inner Tcl interp by name. Tcl
// calls them via `::surftcl::js::call NAME ARG_LIST`; the list elements arrive
// on the JS side as a single array argument of strings. The Tcl bridge is
// type-blind — all marshalling and any application-level type checking is
// up to the JS function. See opt/wacl.c for the C-side protocol.
//
// The host may revoke any granted function. This is the lever for the
// bootstrap-then-seal pattern: page registers `eval` (and whatever else it
// wants), Tcl bootstrap runs, page revokes `eval` before any untrusted
// script gets to evaluate. SurfTcl is a polite guest — what it can do is
// exactly what the host gave it, and only for as long as the host allows.
//
// Return-value protocol (the JS function's actual return):
//
//   undefined, null      -> Tcl result is "", code is TCL_OK
//   any non-array value  -> Tcl result is String(value), code is TCL_OK
//   [status, value]      -> dispatch on status:
//     number n             -> Tcl return code is n. 0=OK, 1=ERROR,
//                             2=RETURN, 3=BREAK, 4=CONTINUE; >=5 are custom
//                             codes that `catch` can pick up.
//     "ok"|"error"|"return"|"break"|"continue"
//                          -> the corresponding code by name (lowercase only).
//     other string         -> TCL_ERROR with ::errorCode = {string}.
//     array of strings     -> TCL_ERROR with ::errorCode = that array.
//   thrown Error           -> TCL_ERROR with the error's message.
//
// Anything richer than a string (objects, arrays of non-strings) is up to
// the caller to serialize — JSON is the obvious default and is in the
// ecosystem already. The bridge moves strings.

// DEFER(js bridge rework) This protocol is complex and error-prone.
//   there's a lot of implicit type conversion.

// DEFER(error handling) Failure to adhere to the Return-value protocol should
//   bubble up as a JS exception and unwind the Tcl interpreter.
//   Possibly as a TclPanic.

export const JsFunctionRegistry = {
  _functions: new Map(),

  register(name, fn) {
    if (typeof name !== 'string')   throw new TypeError('register: name must be a string');
    if (typeof fn   !== 'function') throw new TypeError('register: fn must be a function');
    if (this._functions.get(name) !== undefined) {
      this._revokeJsFn(name);
      Module.removeFunction(this._functions.get(name));
      this._functions.delete(name);
    }

    let fnPtr = Module.addFunction((argc, argvPtr) => this.call(fn, argc, argvPtr), 'iii');
    this._functions.set(name, fnPtr);
    Runtime._registerJsFn(name, fnPtr);
  },

  revoke(name) {
    Runtime._revokeJsFn(name);
    if (this._functions.get(name) !== undefined) {
      Module.removeFunction(this._functions.get(name));
      this._functions.delete(name);
      return 1;
    }
    return 0;
  },

  names() {
    return this._functions.keys();
  },

  call(fn, argc, argvPtr) {
    /* wraps values returned by functions so that they are readable in Tcl */
    // argvPtr points at a contiguous array of i32 C-string pointers in
    // wasm linear memory. Dereference each and decode as UTF-8.
    let args = new Array(argc);
    for (var i = 0; i < argc; i++) {
      let strPtr = Module.getValue(argvPtr + i * 4, 'i32');
      args[i] = Module.UTF8ToString(strPtr);
    }

    let r;
    try {
      let raw = fn(args);
      r = this.normalizeResult(raw);
    } catch (e) {
      // DEFER(error handling) differentiate between JS errors and SurfTcl API errors
      Runtime._setJsResult((e && e.message) ? e.message : String(e));
      return 1;
    }
    Runtime._setJsResult(r.value);
    if (r.errorCode) {
      for (let i = 0; i < r.errorCode.length; i++) {
        Runtime._appendJsErrorCode(r.errorCode[i]);
      }
    }
    return r.code;
  },

  normalizeResult(raw) {
    // Turns a JS value into an Array which this.call() unpacks into Tcl return values.
    if (raw === undefined || raw === null) {
      return { code: 0, value: '', errorCode: null };
    }
    if (!Array.isArray(raw)) {
      return { code: 0, value: String(raw), errorCode: null };
    }
    if (raw.length !== 2) {
      throw new Error('surftcl JS bridge: returned array must be [status, value]');
    }
    var status = raw[0];
    var v = raw[1];
    var valueStr = (v === undefined || v === null) ? '' : String(v);
    if (typeof status === 'number') {
      return { code: status | 0, value: valueStr, errorCode: null };
    }
    if (typeof status === 'string') {
      switch (status) {
        case 'ok':       return { code: 0, value: valueStr, errorCode: null };
        case 'error':    return { code: 1, value: valueStr, errorCode: null };
        case 'return':   return { code: 2, value: valueStr, errorCode: null };
        case 'break':    return { code: 3, value: valueStr, errorCode: null };
        case 'continue': return { code: 4, value: valueStr, errorCode: null };
        default:         return { code: 1, value: valueStr, errorCode: [status] };
      }
    }
    if (Array.isArray(status)) {
      return { code: 1, value: valueStr, errorCode: status.map(String) };
    }
    throw new Error('surftcl JS bridge: status must be a number, string, or string array');
  },
}

// -------------------------------------------------------------------------

Module['postRun'] = () => {
  Runtime = {
    Module: Module,
    _getStringResult: Module.cwrap('SurfTcl_GetStringResult', 'string', ['number']),
    _getInterp:       Module.cwrap('SurfTcl_GetInterp',       'number', []),
    _eval:            Module.cwrap('SurfTcl_Eval',            'number', ['number', 'string']),

    stdin:  stdin,
    stdout: stdout,
    stderr: stderr,

    // The thinnest sensible wrapper around Tcl_EvalEx: pass the script,
    // get rc + result. On error, fetch ::errorInfo for the trace before
    // throwing, since asking for it later would mean re-running Tcl when
    // the page just wants to print what went wrong.
    Eval: (script) => {
      let interp = Runtime._getInterp();
      let rc = Runtime._eval(interp, script);
      if (rc !== 0) {
        let msg = Runtime._getStringResult(interp);
        Runtime._eval(interp, 'set ::errorInfo');
        let trace = Runtime._getStringResult(interp);
        Runtime._eval(interp, 'set ::errorCode');
        let code = Runtime._getStringResult(interp);
        throw new TclException(code, msg, trace);
      }
      return Runtime._getStringResult(interp);
    },

    js: JsFunctionRegistry,
    _appendJsErrorCode: Module.cwrap('SurfTcl_AppendJsErrorCodeElement',   null,   ['string']),
    _setJsResult:       Module.cwrap('SurfTcl_SetJsResultString',          null,   ['string']),
    _registerJsFn:      Module.cwrap('SurfTcl_RegisterJsFn',             'number', ['string', 'number']),
    _revokeJsFn:        Module.cwrap('SurfTcl_RevokeJsFn',               'number', ['string']),

    GrantEval() {
      // Grants `surftcl::js::call eval` privileges to the interpreter.
      // scoped to this module such that `surftcl` is bound to the runtime after
      // initialisation and globalThis refers to the SurfTcl module, not the page.
      // TODO(dther) The scoping is potentially surprising. I need to document it,
      // and explain the escape hatch: `(0, eval)(...)`
      let surftcl = this;
      this.js.register("eval", (args) => {
        let r = eval(args[0]);
        if (r === undefined) return "";
        if (typeof r === "object") return JSON.stringify(r);
        return String(r);
      });
    },

    RevokeEval() { this.js.revoke("eval") },
    RevokeEvalPermanently() {
      this.GrantEval = () => {
        throw new Error("Can't re-grant eval after SurfTcl.RevokeEvalPermanently()");
      };
      this.RevokeEval();
    },

    // FIXME(dther) this error surface doesn't work right, yet. Ironic.
    // Failure surface. The floor is honesty, not an implicit white lie.
    //
    // The default onError handler fires three channels: stderr (visible
    // in the terminal if wired, console.error otherwise), the JS console
    // (implicit via stderr's fallback), and an alert() that names a
    // contact point. If the page set `surftcl.supportURL`, the alert tells
    // the user where to report. If not, the alert confesses that the
    // developer didn't name one — which is the truthful state of the
    // world, and the kind of pressure that gets supportURL set.
    //
    // Pages can override `surftcl.onError` to route errors anywhere they
    // want (Sentry, an in-app toast, /dev/null). Overriding IS the
    // acceptance of responsibility — the floor moves with the developer's
    // explicit choice, never silently.
    //
    // Projects that ship the "developer has not named a point of
    // contact" alert are unsupported by upstream until they either
    // set supportURL or replace onError. Both are easy. The default
    // is calibrated to make either choice obvious.
    supportURL: null,

    onError(context, error) {
      var msg = "[" + context + "] " + ((error && error.message) || String(error));
      stderr.sink("surftcl error: " + msg + "\n");
      if (typeof alert === "function") {
        if (this.supportURL) {
          alert("A fatal surftcl error has occurred.\n\n" +
                "Please report it via: " + this.supportURL +
                "\n\nDetails: " + msg);
        } else {
          alert("A fatal surftcl error has occurred but the developer " +
                "has not named a point of contact through " +
                "surftcl.supportURL.\n\nDetails: " + msg);
        }
      }
    }
  };

  // Finally, so that we can access this runtime from the WASM side...
  Module['SurfTcl'] = Runtime;
}

await createSurfTcl(Module);
export default Runtime;
