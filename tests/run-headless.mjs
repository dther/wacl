#!/usr/bin/env node
// Headless test runner. Imports the built ES6 module from
// wacl-minimal-demo/ (where `make minimal` puts surftcl-bootstrap.mjs
// next to surftcl.mjs + surftcl.wasm), injects the package zips and the
// .test files into the in-wasm FS, then sources tests/all.tcl and exits
// 0 if all tests pass, non-zero otherwise. Same harness the browser
// test page runs, just without the browser.
//
// Usage:  node tests/run-headless.mjs   (or `make test`, which builds first)

import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const here    = path.dirname(fileURLToPath(import.meta.url));
const root    = path.resolve(here, '..');
const demoDir = path.join(root, 'wacl-minimal-demo');

const bootstrap = path.join(demoDir, 'surftcl-bootstrap.mjs');
if (!fs.existsSync(bootstrap) ||
    !fs.existsSync(path.join(demoDir, 'surftcl.mjs'))) {
  console.error('missing built module in wacl-minimal-demo/ — run `make` first');
  process.exit(2);
}

// The module top-level-awaits wasm instantiation: by the time this import
// resolves, main() has run and the default export (the Runtime) is live.
const surftcl = await import(pathToFileURL(bootstrap));
const interp = surftcl.default;

interp.stdout.sink = (t) => process.stdout.write(t);
interp.stderr.sink = (t) => process.stderr.write(t);

// Grant `eval` — every wacl-* package needs it for its JS shim install.
// GrantEval (not a hand-rolled eval) so the shims see the module scope
// they expect: `this` as the Runtime, TclResult, the stdio objects.
interp.GrantEval();

const FS = interp.Module.FS;

// Packages are loaded the way a real consumer loads them: from the
// release zips the ext/ pipeline builds, mounted via zipfs and added
// to auto_path. So this runner validates the actual shipped artifact
// — if a zip is malformed or a package fails to load from one, the
// suite goes red. Build them first with `make -C ext` (or `make
// test` at the repo root, which does both).
const pkgNames = ['wacl-json', 'wacl-dom', 'wacl-chan'];
const extBuild = path.join(root, 'ext', 'build');
try { FS.mkdirTree('/zips'); } catch (e) {}
for (const name of pkgNames) {
  const zipPath = path.join(extBuild, name + '.zip');
  if (!fs.existsSync(zipPath)) {
    console.error(`missing ${zipPath} — run 'make -C ext' to build the package zips first`);
    process.exit(2);
  }
  const dest = '/zips/' + name + '.zip';
  FS.writeFile(dest, fs.readFileSync(zipPath));
  interp.Eval(`tcl::zipfs::mount ${dest} //zipfs:/pkg/${name}`);
  interp.Eval(`lappend auto_path //zipfs:/pkg/${name}`);
}

// Tests live at repo root under tests/; mirror them at /tests in-wasm.
const testFiles = [
  'tests/all.tcl',
  'tests/wacl-json.test',
  'tests/wacl-dom.test',
  'tests/wacl-chan.test',
  'tests/wacl-bridge.test',
];
for (const p of testFiles) {
  const full = '/' + p;
  const dir = full.substring(0, full.lastIndexOf('/'));
  try { FS.mkdirTree(dir); } catch (e) {}
  FS.writeFile(full, fs.readFileSync(path.join(root, p), 'utf8'));
}

try {
  interp.Eval('source /tests/all.tcl');
} catch (e) {
  console.error('\nharness error: ' + (e.errorInfo || e.message || e));
  process.exit(2);
}

const failed = +interp.Eval('set ::tcltest::numTests(Failed)');
process.exit(failed > 0 ? 1 : 0);
