
define('tcl/wacl', function () {
  var _Interp = null;
  var _getInterp = null;
  var _eval = null;
  var _getStringResult = null;
  var _Result = null;

  var _OnReadyCb = function (obj) {};
  
  var _TclException = function(errCode, errInfo) {
    this.errorCode = errCode;
    this.errorInfo = errInfo;
    this.toString = function() {
      return "TclException: " + this.errorCode + " => " + this.errorInfo;
    }
  }
  
  var _currPath = require.toUrl('tcl/');
  var _wasmbly = (function (url) {
    return new Promise(function(resolve, reject) {
      var wasmXHR = new XMLHttpRequest();
      wasmXHR.open('GET', url, true);
      wasmXHR.responseType = 'arraybuffer';
      wasmXHR.onload = function() { resolve(wasmXHR.response); }
      wasmXHR.onerror = function() { reject('error '  + wasmXHR.status); }
      wasmXHR.send(null);
    });
  })( _currPath + 'wacl.wasm');
  
  var Module;
  if (typeof Module === 'undefined') Module = eval('(function() { try { return Module || {} } catch(e) { return {} } })()');
  
  // Stdout/stderr are routed through mutable sinks. Emscripten's runtime
  // captures `out = Module.print` once (during run(), after wasm load), so
  // a later `Module.print = fn` would be ignored. By making Module.print a
  // stable wrapper that delegates to a swappable variable, the setters in
  // _Result below can redirect output at any time.
  var _stdoutSink = function (txt) { console.log('wacl stdout: ' + txt); };
  var _stderrSink = function (txt) { console.error('wacl stderr: ' + txt); };

  Module['noInitialRun'] = false;
  Module['noExitRuntime'] = true;
  Module['print']    = function (txt) { _stdoutSink(txt); };
  Module['printErr'] = function (txt) { _stderrSink(txt); };
  Module['filePackagePrefixURL'] = _currPath;
  
  Module['instantiateWasm'] = function(imports, successCallback) {
    _wasmbly.then(function(wasmBinary) {
      var wasmInstantiate = WebAssembly.instantiate(new Uint8Array(wasmBinary), imports).then(function(output) {
        Module.testWasmInstantiationSucceeded = 1;
        successCallback(output.instance);
      }).catch(function(e) {
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
     
      set stdout(fn) {
        _stdoutSink = fn;
      },
      set stderr(fn) {
        _stderrSink = fn;
      },
      get interp() {
        return _Interp;
      },
      
      str2ptr: function (strObj) {
        return Module.allocate(
                    Module.intArrayFromString(strObj), 
                    'i8', 
                    Module.ALLOC_NORMAL);
      },
      
      ptr2str: function (strPtr) {
        return Module.UTF8ToString(strPtr);
      },
     
      jswrap: function(fcn, returnType, argType) {
        var fnPtr = Runtime.addFunction(fcn);
        return "::wacl::jscall " + fnPtr + " " + returnType + " " + argType;
      },
     
      Eval: function(tclStr) {
        _eval(this.interp, 'catch {' + tclStr + '} ::jsResult');
        var errCode = _getStringResult(this.interp);
        if (errCode != 0) {
          _eval(this.interp, 'set ::errorInfo');
          var errInfo = _getStringResult(this.interp);
          throw new _TclException(errCode, errInfo);
        } else {
          _eval(this.interp, 'set ::jsResult');
          return _getStringResult(this.interp); 
        }
      }
    };

    _OnReadyCb(_Result);
  };
