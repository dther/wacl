#!/usr/bin/env node
// Headless test runner. Loads the wasm in a node vm context, injects
// the wacl-* packages and the .test files into the in-wasm FS, then
// sources tests/all.tcl and exits with status 0 if all tests pass,
// non-zero otherwise. Same harness the browser test page runs, just
// without the browser.
//
// Usage:  node tests/run-headless.mjs
// Intent: CI uses this; locally it's faster than the browser path.

import fs from 'fs';
import path from 'path';
import vm from 'vm';
import { fileURLToPath } from 'url';

const here    = path.dirname(fileURLToPath(import.meta.url));
const root    = path.resolve(here, '..');
const demoDir = path.join(root, 'wacl-minimal-demo');

// AMD shim and a fetch surrogate. The wacl bundle does
// XMLHttpRequest('GET', 'wacl.wasm') with a relative URL, which we
// resolve against demoDir.
const __modules = {};
function XHR() {}
XHR.prototype.open = function (m, url) { this.url = url; };
XHR.prototype.send = function () {
  try {
    const buf = fs.readFileSync(path.join(demoDir, this.url));
    this.response = buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength);
    setImmediate(() => this.onload && this.onload());
  } catch (e) {
    setImmediate(() => this.onerror && this.onerror());
  }
};

const sandbox = {
  define: (n, f) => { __modules[n] = f(); },
  require: (deps, cb) => cb.apply(null, deps.map(d => __modules[d])),
  XMLHttpRequest: XHR,
  WebAssembly, Promise, Uint8Array, TextDecoder, TextEncoder,
  console, setTimeout, setImmediate, clearTimeout, queueMicrotask,
  performance: { now: () => Date.now() },
  process: { env: {} },
  Date, Object, Array, String, Number, Math, JSON,
  Error, TypeError, RangeError, Function, Set, Map,
  alert: () => {},
};
sandbox.define.amd = {};
sandbox.require.toUrl = (p) => (p === 'tcl/' ? '' : p);
sandbox.globalThis = sandbox;
sandbox.self = sandbox;
sandbox.window = sandbox;

const ctx = vm.createContext(sandbox);
vm.runInContext(
  fs.readFileSync(path.join(demoDir, 'wacl-minimal.js'), 'utf8'),
  ctx
);

sandbox.require(['tcl/wacl'], (m) => {
  m.onReady((interp) => {
    interp.stdout = (t) => process.stdout.write(t);
    interp.stderr = (t) => process.stderr.write(t);

    // Grant `eval` — every wacl-* package needs it for its JS shim
    // install. (CI mirrors the production bootstrap shape.)
    vm.runInContext(`
      wacl.js.register("eval", function (args) {
        var r = (0, eval)(args[0]);
        return r === undefined ? "" :
               (typeof r === "object" ? JSON.stringify(r) : String(r));
      });
    `, ctx);

    // Inject a file from disk into the wasm FS at the same path.
    const FS = interp.Module.FS;
    function inject(relPath, srcDir) {
      const body = fs.readFileSync(path.join(srcDir, relPath), 'utf8');
      const full = '/' + relPath;
      const dir = full.substring(0, full.lastIndexOf('/'));
      try { FS.mkdirTree(dir); } catch (e) {}
      FS.writeFile(full, body);
    }

    // Packages live under the demo dir (that's where the deployable
    // copies sit). Tests live at repo root under tests/.
    const packageFiles = [
      'packages/wacl-json/pkgIndex.tcl',
      'packages/wacl-json/wacl-json.tcl',
      'packages/wacl-dom/pkgIndex.tcl',
      'packages/wacl-dom/wacl-dom.tcl',
      'packages/wacl-chan/pkgIndex.tcl',
      'packages/wacl-chan/wacl-chan.tcl',
    ];
    const testFiles = [
      'tests/all.tcl',
      'tests/wacl-json.test',
      'tests/wacl-dom.test',
      'tests/wacl-chan.test',
    ];

    for (const p of packageFiles) inject(p, demoDir);
    for (const p of testFiles)    inject(p, root);
    interp.Eval('lappend auto_path /packages');

    try {
      interp.Eval('source /tests/all.tcl');
    } catch (e) {
      console.error('\nharness error: ' + (e.errorInfo || e.message || e));
      process.exit(2);
    }

    const failed = +interp.Eval('set ::tcltest::numTests(Failed)');
    process.exit(failed > 0 ? 1 : 0);
  });
});
