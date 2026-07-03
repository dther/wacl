// SurfTcl module entry point
// Performs the necessary setup for the SurfTcl Runtime,
// exporting the "Runtime" object by default, which contains
// SurfTcl's public JS API.

import createSurfTcl from "./surftcl.mjs"
export const VERSION = '0.0.0'

// The Module object configures Emscripten glue code,
// and will be further populated at runtime.
//
// See the following docs for more info:
//   https://emscripten.org/docs/api_reference/module.html
export const Module = {
  noInitialRun: false,
  noExitRuntime: true,
};

// Runtime contains all of our runtime-accessible parameters and functions.
// this gets populated with functions by Module['postRun'].
export let Runtime = null;

export class TclException extends Error {
  // TclException: All interpreter thrown exceptions are this class.
  static BRAND = Symbol.for("surftcl.exception");

  constructor (errorCode, errorMessage, errorInfo, options = {}) {
    let message = `SurfTcl.TclException: ${errorCode}\n${errorInfo || errorMessage}`
    super(message, options);

    // for catchers expecting a Tcl error
    this[TclException.BRAND] = true;
    this.errorCode = errorCode; // Tcl list of error codes, e.g., {POSIX NOENT}
    this.errorMessage = errorMessage;  // immediate message from the interp
    this.errorInfo = errorInfo || errorMessage;  // full ::errorInfo trace
  };

  static [Symbol.hasInstance](x) {
    return x != null && x[TclException.BRAND] === true;
  }
};

export class TclPanic extends Error {
  /* Errors that unwind the interpreter are Tcl Panics, and are unrecoverable
   * unless explicity caught and wrapped by the JS runtime.
   *
   * The Tcl interpreter may throw this itself as a result of Tcl_Panic.
   * In that case, the cause will be the string "Tcl_Panic Called".
   */
  // TODO (dther) doesn't do anything right now
  // TODO (dther) special Tcl_Panic handler needs to be set in the C side
  static BRAND = Symbol.for("surftcl.panic");

  constructor (msg, options = { cause: 'unknown' }) {
    let message = `!! SurfTcl PANIC !! ${panic}\n${options.cause ?? ''}`
    super(message, options);
    this[TclPanic.BRAND] = true;
  }

  static [Symbol.hasInstance](x) {
    return x != null && x[TclPanic.BRAND] === true;
  }
};

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

export const JsFunctionRegistry = {
  /* SurfTcl.JsFunctionRegistry manages the capability to execute JS from within
   * the Tcl interpreter.
   *
   * The host (page) grants JS functions to the inner Tcl interp by name. Tcl
   * calls them via `::surftcl::js::call NAME ARG_LIST`; the list elements arrive
   * on the JS side as a single array argument of strings. The Tcl bridge is
   * type-blind — all marshalling and any application-level type checking is
   * up to the JS function. See opt/wacl.c for the C-side protocol.
   *
   * The host may revoke any granted function. This is the lever for the
   * bootstrap-then-seal pattern: page registers `eval` (and whatever else it
   * wants), Tcl bootstrap runs, page revokes `eval` before any untrusted
   * script gets to evaluate. SurfTcl is a polite guest — what it can do is
   * exactly what the host gave it, and only for as long as the host allows.
   *
   * Return-value protocol (the JS function's actual return):
   *
   *   TclResult object   -> Sets return options directly. See the TclResult object.
   *                         Throws its own Errors on use.
   *   undefined, null    -> Tcl result is "", code is TCL_OK
   *   any other value    -> Tcl result is String(value), code is TCL_OK,
   *                         unless value fails to be converted to a String,
   *                         in which case, TCL_ERROR with code
   *                         `SURFTCL JS BADTYPE ${Object.prototype.toString.call(value)}`.
   *   thrown Error/Primitive -> TCL_ERROR with a result based on String(e),
   *                          and error code is set to
   *                          `SURFTCL JS THREW ${e.name ?? Object.prototype.toString.call(e)}`
   *
   * Anything richer than a string (objects, arrays of non-strings) is up to
   * the caller to serialize — JSON is the obvious default and is in the
   * ecosystem already. The bridge moves strings.
   */

  // DEFER(pledge) A wrapper around bootstrap-then-seal that makes a lot of sense is "pledging."
  // Once JS has granted eval, `surftcl::pledge` could be passed a list of capabilities as arguments,
  // like so: `surftcl::pledge dom json chan etc`
  // This command automatically sources the correct versions for these SurfTcl packages,
  // then seals itself by calling `RevokeEvalPermanently()`.
  // Pledging with no arguments simply calls `RevokeEvalPermanently()`.
  //
  // This mechanism is cribbed entirely from OpenBSD, where "pledge" is a declaration
  // of all syscalls the program will ever use. Attempting to expand later results in termination.
  // In our case, non-pledged packages simply become inaccessible.
  //
  // `surftcl::pledge eval` should raise an error, directing to the documentation explaining
  // that eval access must be granted from the JS side, and pledging to use maximum permissions
  // is equivalent to not pledging at all.

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
    // Revokes a function. Returns 1 if the function was on our side of the registry,
    // 0 otherwise. It's a number because the WASM side calls this function, too.
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

    let ret;
    try {
      ret = fn(args);
    } catch (e) {
      // DEFER(nested Tcl-Js errors) a special case for handling a TclException
      // would allow for arbitrarily re-entrant calls to return the entire call stack.
      let msg;
      try { msg = `SurfTcl JS call threw ${String(e)}` }
      catch {
        msg = `SurfTcl JS call threw a non-serialisable ${Object.prototype.toString.call(e)}`
      }
      // DEFER(errorInfo support) Errors may have the propery ".stack", which provides
      // a stack trace. The format is unspecified, but in general, can be converted into
      // a helpful string. It is probably a good idea to append it to errorInfo.
      ret = TclResult.error(msg, {
        errorCode: ['SURFTCL', 'JS', 'THREW', e.name ?? Object.prototype.toString.call(e)]
      });
    }

    try {
      if (!(ret instanceof TclResult)) ret = TclResult.ok(ret);
    } catch {
      ret = TclResult.error("SurfTcl JS call returned non-serializable ${Object.prototype.toString.call(ret)}", {
        errorCode: ['SURFTCL', 'JS', 'BADTYPE', Object.prototype.toString.call(ret)]
      });
    }

    Runtime._setJsResult(ret.value);
    if (ret.options.errorCode) {
      for (let i = 0; i < ret.options.errorCode.length; i++) {
        Runtime._appendJsErrorCode(ret.options.errorCode[i]);
      }
    }
    return ret.code;
  },

}

export class TclResult {
  /* TclResult can be used to wrap the returned value of a JS function
   * for finer control over the state of the Tcl interpreter.
   * See the "return" Tcl manual page for more information.
   *
   * "code" must be a numeric Tcl return code, or one of the following strings:
   *   "ok"|"error"|"return"|"break"|"continue"
   *   These have the same meaning as in the Tcl manpage for the "return" command,
   *   and will be normalized as an integer.
   *   You don't need to specify a code if you use one of the `TclResult.ok(...)`
   *   convenience functions.
   *
   * "value" will be converted into a string if it is not already, in one of two ways:
   *   undefined/null  -> ""
   *   everything else -> String(value). Failure will result in a TypeError.
   *  NOTE: Objects and arrays of non-strings will not serialize into JSON by default!
   *  If this is desired, it must be done by the caller.
   *
   * "options" may have the following optional parameters:
   *   errorCode: an Array of strings representing a Tcl error code as described
   *     in the Tcl manual pages for "throw" and "return".
   *     If errorCode is undefined/null and this.code is "error" or 1 (TCL_ERROR),
   *     the errorCode will be ['NONE']. This mirrors Tcl's `error` command.
   *
   */
  static BRAND = Symbol.for("surftcl.result");
  static CODES = {
    /* Directly map to Tcl constants */
    ok: 0,      /* TCL_OK */
    error: 1,   /* TCL_ERROR */
    return: 2,  /* TCL_RETURN */
    break: 3,   /* TCL_BREAK */
    continue: 4 /* TCL_CONTINUE */
  };

  constructor (code, value, options = {}) {
    try {
      this.value = (value === null || value === undefined) ? "" : String(value);
    } catch (e) {
      throw new TypeError('SurfTcl.TclResult could not serialize a value', { cause: e });
    }
    this.code = TclResult.getReturnCode(code); /* normalize return code, may throw */

    this.options = options;
    this.options.errorCode ??= ['NONE'];
    this[TclResult.BRAND] = true;
  };

  static getReturnCode(code) {
    if (typeof code === "number") {
      if (Number.isInteger(code) && code >= 0) return code;
      throw new TypeError(`SurfTcl.TclResult: code must be a positive integer, got: ${code}`);
    }
    if (code in TclResult.CODES) return TclResult.CODES[code];
    throw new TypeError(`SurfTcl.TclResult: unknown return code: ${code}`);
  }

  static ok(value, options)       { return new TclResult("ok",       value, options); };
  static error(value, options)    { return new TclResult("error",    value, options); };
  static return(value, options)   { return new TclResult("return",   value, options); };
  static break(value, options)    { return new TclResult("break",    value, options); };
  static continue(value, options) { return new TclResult("continue", value, options); };

  static [Symbol.hasInstance](x) {
    return x != null && x[TclResult.BRAND] === true;
  }
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
        throw new TclException(code, msg, trace, { cause: script });
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
      // scoped such that `this` and `surftcl` are bound to the runtime after
      // initialisation, and anything in this module is accessible.

      // TODO(dther) The scoping is potentially surprising. It also prevents
      // the accumulation of state by default. I need to document it,
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
