// minimum viable ES6 module!!!
// the idea: surftcl-bootstrap.mjs is the entry point that sets up the JS API
// surftcl.mjs is the Emscripten module encapsulating the runtime directly
// I intend to rename these later, such that this file becomes "surftcl.mjs"
// and there's more than one build. surftcl.mjs just selects based on configuration.

import createSurfTcl from "./surftcl.mjs"

// this gets passed to the factory on ready
let Module = {}

// Annoyingly, throwing a raw Object arguably has a better UX than Error in Chromium.
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

// TODO(dther) A script should block on `gets stdin` and yield to the browser,
// unless they've configured it to be non-blocking.
// This needs to be configured via surftclNotifier.c.
// Blocking reads should stop the event queue from being serviced, but not stop events from being queued.
let decoder = new TextDecoder();
let encoder = new TextEncoder();

// Stdin queue. Runtime.pushStdin(text) appends; the FS.init input callback
// drains one byte at a time. Returning null from the callback means EOF —
// a script that does `gets stdin` with an empty queue gets EOF immediately.
const stdin = {
  queue: [],
  eof: false,

  write(text) {
    // this is to be used by external code.
    if (this.eof) throw Error("can't write to stdin after close");
    const bytes = encoder.encode(text);
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

// I/O sinks. Defaults route to the JS console — the first place a developer
// looks when something's wrong. Pages override via Runtime.stdout = fn and
// Runtime.stderr = fn after onReady. We hand the sink the bytes Tcl emitted
// as text, *including* any trailing newline — same byte stream xterm.js or
// a remote shell would see.

// Output is delivered to FS.init per byte (or null for flush). We
// accumulate per stream until a newline or flush, then hand a decoded
// string to the sink.
const stdout = {
  // TODO(dther) is there a way to find out EOF on stdout or stderr?
  buffer: [],
  flush() {
    if (this.buffer.length === 0) return;
    // FIXME(dther) is this seriously the right way to do it? **AND WHY DOESN'T IT EMIT AN ERROR BY DEFAULT??!**
    let text = decoder.decode(new Uint8Array(this.buffer));
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
    let text = decoder.decode(new Uint8Array(this.buffer));
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

let _setJsResult       = null;  // wired in postRun
let _appendErrorCodeEl = null;
let _registerJsFn      = null;
let _revokeJsFn        = null;

// name -> Emscripten function-table index, used so revoke() can free the
// slot via removeFunction. The same map drives Runtime.js.names() so we
// don't have to round-trip into Tcl for introspection.
var _jsTableMap = Object.create(null);

function _normalizeJsResult(raw) {
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
}

function _readArgv(argc, argvPtr) {
  // argvPtr points at a contiguous array of i32 C-string pointers in
  // wasm linear memory. Dereference each and decode as UTF-8.
  var args = new Array(argc);
  for (var i = 0; i < argc; i++) {
    var strPtr = Module.getValue(argvPtr + i * 4, 'i32');
    args[i] = Module.UTF8ToString(strPtr);
  }
  return args;
}

function _makeJsShim(userFn) {
  return function (argc, argvPtr) {
    var args = _readArgv(argc, argvPtr);
    var raw;
    try {
      raw = userFn(args);
    } catch (e) {
      _setJsResult((e && e.message) ? e.message : String(e));
      return 1;
    }
    var r;
    try {
      r = _normalizeJsResult(raw);
    } catch (e) {
      _setJsResult(e.message);
      return 1;
    }
    _setJsResult(r.value);
    if (r.errorCode) {
      for (var i = 0; i < r.errorCode.length; i++) {
        _appendErrorCodeEl(r.errorCode[i]);
      }
    }
    return r.code;
  };
}

// -------------------------------------------------------------------------

Module['noInitialRun'] = false;
Module['noExitRuntime'] = true;

// FS.init must be wired in preRun so /dev/stdin, /dev/stdout, /dev/stderr
// are devices backed by callbacks before main() runs and Tcl opens them.
Module['preRun'] = function () {
  Module.FS.init(
    // bind is necessary because of how the "this" keyword works
    stdin._read_callback.bind(stdin),
    stdout._write_callback.bind(stdout),
    stderr._write_callback.bind(stderr)
  );
};

let _Interp = null;
let _getInterp = null;
let _eval = null;
let _getStringResult = null;
let Runtime = null;
Module['postRun'] = function () {
  _getInterp         = Module.cwrap('SurfTcl_GetInterp',                'number', []);
  _eval              = Module.cwrap('SurfTcl_Eval',                     'number', ['number', 'string']);
  _getStringResult   = Module.cwrap('SurfTcl_GetStringResult',          'string', ['number']);
  _setJsResult       = Module.cwrap('SurfTcl_SetJsResultString',          null,   ['string']);
  _appendErrorCodeEl = Module.cwrap('SurfTcl_AppendJsErrorCodeElement',   null,   ['string']);
  _registerJsFn      = Module.cwrap('SurfTcl_RegisterJsFn',             'number', ['string', 'number']);
  _revokeJsFn        = Module.cwrap('SurfTcl_RevokeJsFn',               'number', ['string']);
  _Interp = _getInterp();

  Runtime = {
    Module: Module,

    set stdout(fn) { stdout.sink = fn; },
    set stderr(fn) { stderr.sink = fn; },

    pushStdin: stdin.write.bind(stdin),
    closeStdin: stdin.close.bind(stdin),

    get interp() { return _Interp; },

    // JS function registry. See the top-of-file comment for the protocol.
    // register replaces any prior binding under the same name; revoke is
    // safe to call for names that aren't registered. names() returns the
    // currently-granted names as a JS array.
    js: {
      register(name, fn) {
        if (typeof name !== 'string')   throw new TypeError('register: name must be a string');
        if (typeof fn   !== 'function') throw new TypeError('register: fn must be a function');
        if (_jsTableMap[name] !== undefined) {
          _revokeJsFn(name);
          Module.removeFunction(_jsTableMap[name]);
          delete _jsTableMap[name];
        }
        var fnPtr = Module.addFunction(_makeJsShim(fn), 'iii');
        _jsTableMap[name] = fnPtr;
        _registerJsFn(name, fnPtr);
      },
      revoke: function (name) {
        _revokeJsFn(name);
        if (_jsTableMap[name] !== undefined) {
          Module.removeFunction(_jsTableMap[name]);
          delete _jsTableMap[name];
        }
      },
      names: function () {
        return Object.keys(_jsTableMap);
      },
    },

    // Grants `surftk::js::call eval` privileges to the interpreter.
    // Intentionally scoped to this module, such that `surftcl` is bound to
    // the runtime after initialisation.
    GrantEval() {
      let surftcl = this;
      this.js.register("eval", function (args) {
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

    Eval: TclEval,

    // TODO(dther) EvalAsync needs re-considering.
    // Namely, *it is basically never a good idea to call it directly.*
    // Yielding logic is incredibly complex at the moment. I don't know how to explain it.
    // I think I need to rework the entire Notifier...
    // Should be...
    // - renamed to something else (ServiceEvents()?)
    // - have an obvious one-function way to set it up
    //   (the demos manually patch it in every time)
    // - generally not something the user cares about at all, because we handle the event loop
    //   in most cases

    // Async sibling of Eval — the top-level entry for scripts that may
    // YIELD (`::surftcl::js::yield`, or the `update` wrapper). Returns a
    // Promise: under the Asyncify build a yielding script unwinds the
    // wasm stack to the JS event loop and the Promise resolves once it
    // resumes; a non-yielding script resolves right away. Drive user
    // input through this. Keep using the synchronous Eval above for
    // re-entrant/internal calls that need the result immediately and
    // are known not to yield (the JS bridge re-enters that way, and a
    // synchronous ccall can't survive an unwind). Error handling
    // mirrors Eval; the ::errorInfo fetch is itself a non-yielding eval.
    EvalAsync(script) {
      var interp = this.interp;
      return Promise.resolve(
        Module.ccall('SurfTcl_Eval', 'number', ['number', 'string'],
                     [interp, script], { async: true })
      ).then(function (rc) {
        if (rc !== 0) {
          var msg = _getStringResult(interp);
          _eval(interp, 'set ::errorInfo');
          var trace = _getStringResult(interp);
          throw new TclException(rc, msg, trace);
        }
        return _getStringResult(interp);
      });
    },

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
}

// The thinnest sensible wrapper around Tcl_EvalEx: pass the script,
// get rc + result. On error, fetch ::errorInfo for the trace before
// throwing, since asking for it later would mean re-running Tcl when
// the page just wants to print what went wrong.
function TclEval(script) {
  let rc = _eval(_Interp, script);
  if (rc !== 0) {
    let msg = _getStringResult(_Interp);
    _eval(_Interp, 'set ::errorInfo');
    let trace = _getStringResult(_Interp);
    _eval(_Interp, 'set ::errorCode');
    let code = _getStringResult(_Interp);
    throw new TclException(code, msg, trace);
  }
  return _getStringResult(_Interp);
}

// this should work, right??
export async function onReady(func) {
  await createSurfTcl(Module);
  func(Runtime);
}
