# SurfTcl build — Tcl 9 in the browser.
#
# Targets that matter:
#   make tcl          download and unpack Tcl 9 source under ./tcl/
#   make minimal      build wacl-minimal.{js,wasm} (Asyncify/yield-capable,
#                     ~4MB) and copy them into wacl-minimal-demo/
#   make packages     build the per-package release zips under ext/build/
#   make test         build the package zips, then run the headless suite
#   make clean        remove build artefacts but keep ./tcl/
#   make distclean    also remove ./tcl/
#   make fullclean    remove the Tcl source tar, too
#
# The extensions/all/install/package targets that were here for the
# 8.6 era (with tdom + rl_json + tcllib) are gone for now; they'll
# come back once tdom has been re-patched against Tcl 9 and the
# extension story has been thought through. See CLAUDE.md.

TCLVERSION ?= 9.0.3
TCLSRC      = tcl$(TCLVERSION)-src.tar.gz
TCLURL      = https://prdownloads.sourceforge.net/tcl/$(TCLSRC)

BCFLAGS ?= -Oz -s WASM=1
#BCFLAGS ?= -O0 -g4 -s WASM=1

WASMFLAGS_MINIMAL = \
    --pre-js preGeneratedJs.js --post-js js/postJsRequire.js $(BCFLAGS) \
    -s FORCE_FILESYSTEM=1 \
    -s ALLOW_TABLE_GROWTH=1 \
    -s EXPORTED_RUNTIME_METHODS='["cwrap","ccall","FS","addFunction","removeFunction","getValue","UTF8ToString"]' \
    --embed-file tcl/unix/libtcl9.0.3.zip@/lib/tcl.zip

SURFTCLEXPORTS = \
    -s EXPORTED_FUNCTIONS="[\
        '_main',\
        '_SurfTcl_GetInterp',\
        '_SurfTcl_Eval',\
        '_SurfTcl_GetStringResult',\
        '_SurfTcl_RegisterJsFn',\
        '_SurfTcl_RevokeJsFn',\
        '_SurfTcl_SetJsResultString',\
        '_SurfTcl_AppendJsErrorCodeElement',\
        '_SurfTcl_ServiceEvents'\
    ]"

SURFTCLCC = \
    -I tcl/unix -I tcl/generic -I tcl/libtommath -I opt $(BCFLAGS) \
    -DSTATIC_BUILD=1 -DBUILD_tcl -DTCL_THREADS=0

.PHONY: minimal packages test clean distclean fullclean

default: minimal

# The package pipeline lives in ext/. The headless suite loads packages
# from the zips it produces, so `test` builds them first.
packages:
	$(MAKE) -C ext

test: packages
	node tests/run-headless.mjs

tcl:
	wget -nc $(TCLURL)
	mkdir -p tcl
	tar -C tcl --strip-components=1 -xf $(TCLSRC)

# Tcl 9's Makefile.in has two issues that we patch in-place after configure:
#
#  1. CC_SWITCHES doesn't include ZLIB_INCLUDE. When configure decides to
#     use bundled compat/zlib (the Emscripten sysroot has no system zlib),
#     generic .c files that #include "zlib.h" can't find it. Upstream bug,
#     worth a patch eventually.
#
#  2. -DTCL_THREADS=0 isn't reachable from configure flags any more — Tcl 9
#     removed --disable-threads, and TCL_THREADS defaults to 1 in tclInt.h.
#     Threads pull in pthread_kill, which Emscripten doesn't provide because
#     wasm/WebWorkers don't have POSIX signals. We force-disable here.
tcl/unix/Makefile: tcl
	cd tcl/unix && emconfigure ./configure --disable-load --disable-shared
	cd tcl/unix && sed -i \
	    's|^CC_SWITCHES = $$(STUB_CC_SWITCHES) -DBUILD_tcl|CC_SWITCHES = $$(STUB_CC_SWITCHES) $${ZLIB_INCLUDE} -DTCL_THREADS=0 -DBUILD_tcl|' Makefile

tcl/unix/libtcl9.0.a: tcl/unix/Makefile
	cd tcl/unix && emmake make libtcl9.0.a

# The baseline build is Asyncify-enabled: -DSURFTCL_ASYNCIFY turns on
# `::surftcl::js::yield` (emscripten_sleep-backed) and -sASYNCIFY instruments
# the module so a synchronous Tcl call can unwind to the JS event loop and
# resume in place. This is what makes `interp.Eval` async and the `update`
# wrapper work — the event-loop story SurfTcl is built on (docs/event-loop.md).
# It costs ~1.5x size (≈4MB) and a speed tax; Binaryen instruments broadly
# because Tcl's function pointers defeat call-graph scoping. JSPI is the
# lighter successor once it's cross-browser — the C and the `update` wrapper
# are mechanism-agnostic, so that swap is localized.
minimal: tcl/unix/libtcl9.0.a
	emcc -c $(SURFTCLCC) -DSURFTCL_ASYNCIFY opt/wacl.c -o surftcl.o
	emcc -c $(SURFTCLCC) -DSURFTCL_ASYNCIFY opt/waclNotifier.c -o waclNotifier.o
	emcc -c $(SURFTCLCC) opt/waclAppInit.c -o waclAppInit.o
	cp js/preJsRequire.js preGeneratedJs.js
	emcc $(WASMFLAGS_MINIMAL) $(SURFTCLEXPORTS) \
	    -sASYNCIFY -sASYNCIFY_STACK_SIZE=1048576 \
	    surftcl.o waclNotifier.o waclAppInit.o tcl/unix/libtcl9.0.a \
	    -o wacl-minimal.js
	cp wacl-minimal.js wacl-minimal.wasm wacl-minimal-demo/

clean:
	rm -f *.o wacl-minimal.js wacl-minimal.wasm preGeneratedJs.js
	if [ -e tcl/unix/Makefile ] ; then cd tcl/unix && make clean ; fi

# We don't ever change the Tcl source tarball directly,
# so preserve it by default to save bandwidth.
distclean: clean
	rm -rf tcl

fullclean: distclean
	rm -f $(TCLSRC)
