
define('tcl/wacl', function () {
  var _Interp = null;
  var _getInterp = null;
  var _eval = null;
  var _getStringResult = null;
  var _Result = null;

  var _OnReadyCb = function (obj) {};

  var _TclException = function (errCode, errMessage, errInfo) {
    this.errorCode = errCode;        // numeric: TCL_ERROR etc.
    this.errorMessage = errMessage;  // immediate message from the interp
    this.errorInfo = errInfo || errMessage;  // full ::errorInfo trace
    this.toString = function () {
      return 'TclException: ' + this.errorMessage;
    };
  };

  var _currPath = require.toUrl('tcl/');
  var _wasmbly = (function (url) {
    return new Promise(function (resolve, reject) {
      var wasmXHR = new XMLHttpRequest();
      wasmXHR.open('GET', url, true);
      wasmXHR.responseType = 'arraybuffer';
      wasmXHR.onload = function () { resolve(wasmXHR.response); };
      wasmXHR.onerror = function () { reject('error ' + wasmXHR.status); };
      wasmXHR.send(null);
    });
  })(_currPath + 'wacl.wasm');

  var Module;
  if (typeof Module === 'undefined') Module = eval('(function() { try { return Module || {} } catch(e) { return {} } })()');

  // I/O sinks. Defaults route to the JS console — the first place a developer
  // looks when something's wrong. Pages override via _Result.stdout = fn and
  // _Result.stderr = fn after onReady. We hand the sink the bytes Tcl emitted
  // as text, *including* any trailing newline — same byte stream xterm.js or
  // a remote shell would see.
  var _stdoutSink = function (text) { console.log(text); };
  var _stderrSink = function (text) { console.error(text); };

  // Stdin queue. _Result.pushStdin(text) appends; the FS.init input callback
  // drains one byte at a time. Returning null from the callback means EOF —
  // a script that does `gets stdin` with an empty queue gets EOF immediately,
  // so for interactive use the page should push bytes before evaluating
  // anything that reads stdin. Real async stdin is the keypress-stream idea
  // and lives in the future.
  var _stdinQueue = [];
  var _stdinEof = false;

  // Output is delivered to FS.init per byte (or null for flush). We
  // accumulate per stream until a newline or flush, then hand a decoded
  // string to the sink.
  var _outBuf = [];
  var _errBuf = [];
  var _decoder = (typeof TextDecoder !== 'undefined') ? new TextDecoder() : null;

  function _flushBuf(buf, sink) {
    if (buf.length === 0) return;
    var text = _decoder
        ? _decoder.decode(new Uint8Array(buf))
        : String.fromCharCode.apply(null, buf);
    buf.length = 0;
    sink(text);
  }

  function _emit(buf, sink, byte) {
    if (byte === null) { _flushBuf(buf, sink); return; }
    buf.push(byte);
    if (byte === 10 /* \n */) _flushBuf(buf, sink);
  }

  // ---- JS function registry ------------------------------------------------
  //
  // The host (page) grants JS functions to the inner Tcl interp by name. Tcl
  // calls them via `::wacl::js::call NAME ARG_LIST`; the list elements arrive
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

  var _setJsResult       = null;  // wired in postRun
  var _appendErrorCodeEl = null;
  var _registerJsFn      = null;
  var _revokeJsFn        = null;

  // name -> Emscripten function-table index, used so revoke() can free the
  // slot via removeFunction. The same map drives _Result.js.names() so we
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
      throw new Error('wacl JS bridge: returned array must be [status, value]');
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
    throw new Error('wacl JS bridge: status must be a number, string, or string array');
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
  Module['filePackagePrefixURL'] = _currPath;

  // FS.init must be wired in preRun so /dev/stdin, /dev/stdout, /dev/stderr
  // are devices backed by our callbacks before main() runs and Tcl opens
  // them. Doing this here bypasses the older Module.print/printErr hooks,
  // which Emscripten's runtime caches once during run() and which were the
  // source of every "puts isn't reaching my callback" bug in this project.
  Module['preRun'] = function () {
    Module.FS.init(
      function () {
        if (_stdinEof || _stdinQueue.length === 0) return null;
        return _stdinQueue.shift();
      },
      function (b) { _emit(_outBuf, _stdoutSink, b); },
      function (b) { _emit(_errBuf, _stderrSink, b); }
    );
  };

  Module['instantiateWasm'] = function (imports, successCallback) {
    _wasmbly.then(function (wasmBinary) {
      WebAssembly.instantiate(new Uint8Array(wasmBinary), imports).then(function (output) {
        Module.testWasmInstantiationSucceeded = 1;
        successCallback(output.instance);
      }).catch(function (e) {
        // wacl.onError doesn't exist yet (postRun hasn't fired), so inline
        // the same shape — loud, named, honest. Nothing else can work after
        // this fails, so this is exactly the kind of failure the policy is
        // designed for.
        var msg = "wasm instantiation failed: " + e;
        _stderrSink(msg + "\n");
        if (typeof alert === "function") {
          alert("A fatal wacl error has occurred but the developer has not " +
                "named a point of contact through wacl.supportURL.\n\n" +
                "Details: " + msg);
        }
      });
    });
    return {};
  };

  Module['postRun'] = function () {
    _getInterp         = Module.cwrap('Wacl_GetInterp',                'number', []);
    _eval              = Module.cwrap('Wacl_Eval',                     'number', ['number', 'string']);
    _getStringResult   = Module.cwrap('Wacl_GetStringResult',          'string', ['number']);
    _setJsResult       = Module.cwrap('Wacl_SetJsResultString',          null,   ['string']);
    _appendErrorCodeEl = Module.cwrap('Wacl_AppendJsErrorCodeElement',   null,   ['string']);
    _registerJsFn      = Module.cwrap('Wacl_RegisterJsFn',             'number', ['string', 'number']);
    _revokeJsFn        = Module.cwrap('Wacl_RevokeJsFn',               'number', ['string']);
    _Interp = _getInterp();

    _Result = {
      Module: Module,

      set stdout(fn) { _stdoutSink = fn; },
      set stderr(fn) { _stderrSink = fn; },

      pushStdin: function (text) {
        if (_stdinEof) return;
        var bytes = (typeof TextEncoder !== 'undefined')
            ? new TextEncoder().encode(text)
            : (function () {
                var out = new Uint8Array(text.length);
                for (var j = 0; j < text.length; j++) out[j] = text.charCodeAt(j) & 0xff;
                return out;
              })();
        for (var i = 0; i < bytes.length; i++) _stdinQueue.push(bytes[i]);
      },

      closeStdin: function () { _stdinEof = true; },

      get interp() { return _Interp; },

      // JS function registry. See the top-of-file comment for the protocol.
      // register replaces any prior binding under the same name; revoke is
      // safe to call for names that aren't registered. names() returns the
      // currently-granted names as a JS array.
      js: {
        register: function (name, fn) {
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
        }
      },

      // The thinnest sensible wrapper around Tcl_EvalEx: pass the script,
      // get rc + result. On error, fetch ::errorInfo for the trace before
      // throwing, since asking for it later would mean re-running Tcl when
      // the page just wants to print what went wrong.
      Eval: function (script) {
        var rc = _eval(this.interp, script);
        if (rc !== 0) {
          var msg = _getStringResult(this.interp);
          _eval(this.interp, 'set ::errorInfo');
          var trace = _getStringResult(this.interp);
          throw new _TclException(rc, msg, trace);
        }
        return _getStringResult(this.interp);
      },

      // Async sibling of Eval — the top-level entry for scripts that may
      // YIELD (`::wacl::js::yield`, or the `update` wrapper). Returns a
      // Promise: under the Asyncify build a yielding script unwinds the
      // wasm stack to the JS event loop and the Promise resolves once it
      // resumes; a non-yielding script resolves right away. Drive user
      // input through this. Keep using the synchronous Eval above for
      // re-entrant/internal calls that need the result immediately and
      // are known not to yield (the JS bridge re-enters that way, and a
      // synchronous ccall can't survive an unwind). Error handling
      // mirrors Eval; the ::errorInfo fetch is itself a non-yielding eval.
      EvalAsync: function (script) {
        var interp = this.interp;
        return Promise.resolve(
          Module.ccall('Wacl_Eval', 'number', ['number', 'string'],
                       [interp, script], { async: true })
        ).then(function (rc) {
          if (rc !== 0) {
            var msg = _getStringResult(interp);
            _eval(interp, 'set ::errorInfo');
            var trace = _getStringResult(interp);
            throw new _TclException(rc, msg, trace);
          }
          return _getStringResult(interp);
        });
      },

      // Failure surface. The floor is honesty, not an implicit white lie.
      //
      // The default onError handler fires three channels: stderr (visible
      // in the terminal if wired, console.error otherwise), the JS console
      // (implicit via stderr's fallback), and an alert() that names a
      // contact point. If the page set `wacl.supportURL`, the alert tells
      // the user where to report. If not, the alert confesses that the
      // developer didn't name one — which is the truthful state of the
      // world, and the kind of pressure that gets supportURL set.
      //
      // Pages can override `wacl.onError` to route errors anywhere they
      // want (Sentry, an in-app toast, /dev/null). Overriding IS the
      // acceptance of responsibility — the floor moves with the developer's
      // explicit choice, never silently.
      //
      // Projects that ship the "developer has not named a point of
      // contact" alert are unsupported by upstream until they either
      // set supportURL or replace onError. Both are easy. The default
      // is calibrated to make either choice obvious.
      supportURL: null,

      onError: function (context, error) {
        var msg = "[" + context + "] " + ((error && error.message) || String(error));
        _stderrSink("wacl error: " + msg + "\n");
        if (typeof alert === "function") {
          if (this.supportURL) {
            alert("A fatal wacl error has occurred.\n\n" +
                  "Please report it via: " + this.supportURL +
                  "\n\nDetails: " + msg);
          } else {
            alert("A fatal wacl error has occurred but the developer " +
                  "has not named a point of contact through " +
                  "wacl.supportURL.\n\nDetails: " + msg);
          }
        }
      }
    };

    // Bless the wacl handle as a global, defended against accidental
    // shadowing (`var wacl = ...` at page scope would otherwise clobber
    // it silently — JS has no warning for that, and a stray reassignment
    // would break every package that looks the handle up by name).
    // Properties on the object stay mutable; only the binding is locked.
    Object.defineProperty(globalThis, "wacl", {
      value: _Result,
      writable: false,
      configurable: false,
      enumerable: true
    });

    _OnReadyCb(_Result);
  };
