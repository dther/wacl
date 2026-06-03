#!/usr/bin/env node
// Async / yield harness for SurfTcl's event-loop core.
//
// run-headless.mjs drives the tcltest suites *synchronously* — it can't reach
// the part of SurfTcl that only exists on the async path: a script that calls
// `::surftcl::js::yield` (directly, or later via the `update` wrapper / a
// parking `vwait`) unwinds the wasm stack under Asyncify and resumes in place.
// That suspend/resume, the one-suspension guard, and re-entrancy *during* a
// suspension are exactly the things that were awkward to test by hand. This is
// the harness for them: each test awaits `interp.EvalAsync(...)` and asserts on
// what survives a yield.
//
//   Run:   node tests/run-async.mjs          (also wired into `make test`)
//   Needs: the Asyncify wasm at wacl-minimal-demo/wacl-minimal.{js,wasm},
//          built by `make minimal`. Exits non-zero on any failure.
//
// It boots the same wasm the browser does, in a node `vm` context, behind the
// AMD shim and an XMLHttpRequest surrogate that reads wacl.wasm off disk. The
// loader is a trimmed copy of run-headless.mjs's, kept self-contained on
// purpose so the two runners can't break one another.

import fs from 'fs';
import path from 'path';
import vm from 'vm';
import { fileURLToPath } from 'url';

const here    = path.dirname(fileURLToPath(import.meta.url));
const root    = path.resolve(here, '..');
const demoDir = path.join(root, 'wacl-minimal-demo');

// ---- boot the wasm, resolve with the interp handle -----------------------

function loadInterp() {
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

  return new Promise((resolve, reject) => {
    const fail = setTimeout(() => reject(new Error('wasm never reached onReady')), 20000);
    sandbox.require(['tcl/wacl'], (m) => {
      m.onReady((interp) => {
        clearTimeout(fail);
        interp.stdout = () => {};   // quiet by default; a test can re-point
        interp.stderr = () => {};
        resolve({ interp, ctx, sandbox });
      });
    });
  });
}

// ---- a tiny async test framework -----------------------------------------

const tests = [];
function test(name, fn) { tests.push({ name, fn, skip: false }); }
test.skip = (name, why) => tests.push({ name, skip: true, why });

function assert(cond, msg) {
  if (!cond) throw new Error(msg || 'assertion failed');
}
function eq(actual, expected, msg) {
  if (actual !== expected) {
    throw new Error((msg ? msg + ': ' : '') +
      `expected ${JSON.stringify(expected)}, got ${JSON.stringify(actual)}`);
  }
}
// Guard every await: a yield that never resumes should fail the test loudly,
// not wedge the whole harness.
function withTimeout(p, ms, label) {
  let t;
  const guard = new Promise((_, rej) => {
    t = setTimeout(() => rej(new Error(`timed out after ${ms}ms: ${label}`)), ms);
  });
  return Promise.race([p, guard]).finally(() => clearTimeout(t));
}

// ---- the tests -----------------------------------------------------------

test('EvalAsync resolves a non-yielding script', async ({ interp }) => {
  const r = await withTimeout(interp.EvalAsync('expr {6 * 7}'), 5000, 'plain EvalAsync');
  eq(r, '42');
});

test('EvalAsync runs a yielding script through to its result', async ({ interp }) => {
  // ::surftcl::js::yield unwinds to the JS loop (emscripten_sleep) and resumes
  // in place; the Promise resolves with the post-resume result.
  const r = await withTimeout(
    interp.EvalAsync('::surftcl::js::yield; expr {1 + 1}'), 5000, 'yield+resume');
  eq(r, '2');
});

test('a re-entrant non-yielding Eval lands between suspend and resume', async ({ interp }) => {
  interp.Eval('set ::probe before');
  const p = interp.EvalAsync('::surftcl::js::yield; set ::probe');
  // EvalAsync ran the script up to the yield and unwound, so control is back
  // here with the evaluation suspended. A synchronous re-entrant Eval is
  // allowed (it doesn't itself yield); its mutation must be visible on resume.
  interp.Eval('set ::probe after');
  const r = await withTimeout(p, 5000, 'reentrant-during-suspension');
  eq(r, 'after', 'resumed script must see the re-entrant write (which also proves it suspended)');
});

test('the one-suspension guard rejects a nested yield with {SURFTCL YIELD NESTED}',
async ({ interp }) => {
  const p = interp.EvalAsync('::surftcl::js::yield; set ::guard_done yes');
  // Outer is suspended at its yield (surftclYieldInFlight == 1). A re-entrant
  // eval that *itself* yields must be refused before it can put a second
  // unwind in flight — the guard returns an error rather than suspending.
  let threw = false, code = null;
  try {
    interp.Eval('::surftcl::js::yield');
  } catch (e) {
    threw = true;
    code = interp.Eval('set ::errorCode');
  }
  assert(threw, 'a nested yield should raise, not suspend');
  eq(code, 'SURFTCL YIELD NESTED', 'nested-yield errorCode');
  // And the outer evaluation must still resume cleanly — the guard protects
  // it; it is not poisoned by the rejected nested attempt.
  const r = await withTimeout(p, 5000, 'outer-resumes-after-nested-reject');
  eq(r, 'yes');
});

test('EvalAsync rejects a Tcl error, carrying message and trace', async ({ interp }) => {
  let threw = false, ex = null;
  try { await withTimeout(interp.EvalAsync('error {boom}'), 5000, 'error path'); }
  catch (e) { threw = true; ex = e; }
  assert(threw, 'an erroring script should reject');
  assert(/boom/.test(ex.errorMessage || String(ex)), 'message carries the error');
  assert(/boom/.test(ex.errorInfo || ''), 'errorInfo carries the trace');
});

test('SurfTcl_ServiceEvents is reachable from JS and drains to a count',
async ({ interp }) => {
  // The JS-driven pump. With nothing queued it services some number >= 0 and
  // returns the count; later event tests will feed work in and assert it ran.
  const n = interp.Module.ccall('SurfTcl_ServiceEvents', 'number', [], []);
  assert(typeof n === 'number' && n >= 0, `ServiceEvents returns a count, got ${n}`);
});

// ---- pinned current behaviour + future targets ---------------------------
// On the main thread today, an indefinite wait can't park: SurfTclWaitForEvent
// returns -1 for a NULL timeout, so a blocking Tcl_DoOneEvent gives up and
// vwait raises NO_SOURCES instead of hanging the page. This test PINS that, so
// the notifier refactor (make WaitForEvent yield) visibly flips it; the skips
// below are the targets that turn on with it.

test('vwait with no possible source fails fast today (pre-notifier-refactor)',
async ({ interp }) => {
  let threw = false;
  try { await withTimeout(interp.EvalAsync('vwait ::never_set'), 5000, 'vwait-nosources'); }
  catch (e) { threw = true; }
  assert(threw, 'vwait with no source should fail fast today, not hang or park');
  // When WaitForEvent learns to yield, delete this and enable the first skip.
});

test.skip('vwait parks, then resumes when an after-timer sets its variable',
  'needs SurfTclWaitForEvent to yield to the JS loop (notifier refactor)');
test.skip('a redraw yield resumes on requestAnimationFrame and refuses Evals',
  'needs the yield-kind descriptor + the JS-side resume policy');

// ---- run -----------------------------------------------------------------

(async () => {
  console.log('SurfTcl async/yield harness');
  console.log('wasm: ' + path.relative(root, path.join(demoDir, 'wacl-minimal.js')) + '\n');

  let env;
  try {
    env = await loadInterp();
  } catch (e) {
    console.error('harness error: ' + (e.message || e) +
      '\n(build the wasm first: `make minimal`)');
    process.exit(2);
  }

  let passed = 0, failed = 0, skipped = 0;
  for (const t of tests) {
    if (t.skip) { console.log(`  skip  ${t.name}\n        (${t.why})`); skipped++; continue; }
    try { await t.fn(env); console.log(`  ok    ${t.name}`); passed++; }
    catch (e) { console.log(`  FAIL  ${t.name}\n        ${e.message || e}`); failed++; }
  }

  console.log(`\n${passed} passed, ${failed} failed, ${skipped} skipped`);
  process.exit(failed ? 1 : 0);
})();
