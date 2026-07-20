#!/usr/bin/env node
// Panic smoke test. Runs as its own process because a Tcl_Panic abandons
// the wasm stack — the module instance is dead afterwards, which is the
// documented TclPanic contract. Asserts that a panic raised in C surfaces
// in JS as a TclPanic (not an opaque wasm abort) with the message intact.
//
// Usage:  node tests/panic-smoke.mjs   (or `make test`, which builds first)

import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const here    = path.dirname(fileURLToPath(import.meta.url));
const demoDir = path.resolve(here, '..', 'wacl-minimal-demo');

const bootstrap = path.join(demoDir, 'surftcl-bootstrap.mjs');
if (!fs.existsSync(bootstrap)) {
  console.error('missing built module in wacl-minimal-demo/ — run `make` first');
  process.exit(2);
}

const surftcl = await import(pathToFileURL(bootstrap));
const { TclPanic } = surftcl;
const interp = surftcl.default;
interp.stdout.sink = () => {};
interp.stderr.sink = () => {};

let caught = null;
try {
  interp.Module.ccall('SurfTcl_Panic', null, ['string'], ['smoke test panic']);
} catch (e) {
  caught = e;
}

if (caught === null) {
  console.error('SurfTcl_Panic returned instead of throwing');
  process.exit(1);
}
if (!(caught instanceof TclPanic)) {
  console.error('panic did not surface as TclPanic: ' + caught);
  process.exit(1);
}
if (!String(caught.message).includes('smoke test panic')) {
  console.error('panic message lost: ' + caught.message);
  process.exit(1);
}
console.log('panic smoke: TclPanic surfaced with message intact');
process.exit(0);
