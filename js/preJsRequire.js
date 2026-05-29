
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

  Module['noInitialRun'] = false;
  Module['noExitRuntime'] = true;
  Module['filePackagePrefixURL'] = _currPath;

  // FS.init must be wired in preRun so /dev/stdin, /dev/stdout, /dev/stderr
  // are devices backed by our callbacks before main() runs and Tcl opens
  // them. Doing this here bypasses the older Module.print/printErr hooks,
  // which Emscripten's runtime caches once during run() and which were the
  // source of every "puts isn't reaching my callback" bug in this project.
  Module['preRun'] = function () {
    // FS lives inside the runtime closure; we reach it via Module.FS, which
    // requires "FS" in EXPORTED_RUNTIME_METHODS at build time.
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
        console.log('wasm instantiation failed! ' + e);
      });
    });
    return {};
  };

  Module['postRun'] = function () {
    _getInterp = Module.cwrap('Wacl_GetInterp', 'number', []);
    _eval = Module.cwrap('Tcl_Eval', 'number', ['number', 'string']);
    _getStringResult = Module.cwrap('Tcl_GetStringResult', 'string', ['number']);
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

      str2ptr: function (strObj) {
        return Module.allocate(
          Module.intArrayFromString(strObj),
          'i8',
          Module.ALLOC_NORMAL);
      },

      ptr2str: function (strPtr) {
        return Module.UTF8ToString(strPtr);
      },

      jswrap: function (fcn, returnType, argType) {
        var fnPtr = Runtime.addFunction(fcn);
        return '::wacl::jscall ' + fnPtr + ' ' + returnType + ' ' + argType;
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
      }
    };

    _OnReadyCb(_Result);
  };
