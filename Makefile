# SurfTcl build — Tcl 9 in the browser.
#
# Targets that matter:
#   make tcl          download and unpack Tcl 9 source under ./tcl/
#   make minimal      build surftcl.mjs + surftcl.wasm and copy them (with
#                     js/surftcl-bootstrap.mjs) into wacl-minimal-demo/
#   make surftcl-demo build the wasm, then copy the demo site (pages, wasm,
#                     packages, tests) into the surftcl-demo repo
#   make packages     build the per-package release zips under ext/build/
#   make test         build the wasm + zips, then run the tcltest suites
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

# The generated GitHub Pages site lives in its own repo (dther/surftcl-demo),
# rebuilt from this tree by `make surftcl-demo`. Default to a sibling checkout.
DEMOREPO ?= ../surftcl-demo

WASMFLAGS_MINIMAL = \
    $(BCFLAGS) \
    -s MODULARIZE \
    -s EXPORT_ES6 \
    -s EXPORT_NAME=createSurfTcl \
    -s FORCE_FILESYSTEM=1 \
    -s ALLOW_TABLE_GROWTH=1 \
    -s EXPORTED_RUNTIME_METHODS=cwrap,ccall,FS,addFunction,removeFunction,getValue,UTF8ToString \
    --embed-file tcl/unix/libtcl9.0.3.zip@/lib/tcl.zip

SURFTCLEXPORTS = \
    -s EXPORTED_FUNCTIONS="\
        _main,\
        _SurfTcl_GetInterp,\
        _SurfTcl_Eval,\
        _SurfTcl_GetStringResult,\
        _SurfTcl_RegisterJsFn,\
        _SurfTcl_RevokeJsFn,\
        _SurfTcl_SetJsResultString,\
        _SurfTcl_AppendJsErrorCodeElement\
    "

SURFTCLCC = \
    -I tcl/unix -I tcl/generic -I tcl/libtommath -I opt $(BCFLAGS) \
    -DSTATIC_BUILD=1 -DBUILD_tcl -DTCL_THREADS=0

.PHONY: minimal surftcl-demo packages test clean distclean fullclean

default: minimal

# The package pipeline lives in ext/. The headless suite loads packages
# from the zips it produces, and reads the wasm the same build emits into
# wacl-minimal-demo/ — now that the wasm isn't committed, `test` builds it.
packages:
	$(MAKE) -C ext

test: minimal packages
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

# Order-only: the opt/ compiles include headers from the Tcl source tree,
# so on a cold build the tree must be unpacked and configured first. Order-
# only because a re-download/re-configure shouldn't force .o rebuilds.
surftcl.o surftclAppInit.o surftclNotifier.o: | tcl/unix/Makefile

surftcl.o: opt/surftcl.c
	emcc $(SURFTCLCC) -c $^ -o $@

surftclAppInit.o: opt/surftclAppInit.c
	emcc $(SURFTCLCC) -c $^ -o $@

surftclNotifier.o: opt/surftclNotifier.c
	emcc $(SURFTCLCC) -c $^ -o $@

surftcl.mjs: surftcl.o surftclAppInit.o surftclNotifier.o tcl/unix/libtcl9.0.a
	emcc $(WASMFLAGS_MINIMAL) $(SURFTCLEXPORTS) \
	    $^ -o $@

surftcl.wasm: surftcl.mjs

minimal: surftcl.mjs surftcl.wasm js/surftcl-bootstrap.mjs
	cp $^ wacl-minimal-demo/

# Generate the surftcl-demo repo (the GitHub Pages site) from this tree: the
# demo pages, the freshly built wasm, and the package and test files the pages
# fetch at boot. The layout mirrors this tree so the pages' relative fetches
# resolve unchanged; a root index.html redirects to wacl-minimal-demo/. The
# repo is a rebuilt artifact — regenerate and commit it on every change,
# keeping the wasm out of source-tree history. DEMOREPO is the demo checkout.
surftcl-demo: minimal
	mkdir -p $(DEMOREPO)/wacl-minimal-demo/playground \
	         $(DEMOREPO)/wacl-minimal-demo/tests $(DEMOREPO)/tests
	cp wacl-minimal-demo/index.html            $(DEMOREPO)/wacl-minimal-demo/
	cp wacl-minimal-demo/playground/index.html $(DEMOREPO)/wacl-minimal-demo/playground/
	cp wacl-minimal-demo/tests/index.html      $(DEMOREPO)/wacl-minimal-demo/tests/
	cp surftcl.mjs surftcl.wasm js/surftcl-bootstrap.mjs $(DEMOREPO)/wacl-minimal-demo/
	rm -rf $(DEMOREPO)/packages && cp -R packages $(DEMOREPO)/packages
	cp tests/all.tcl tests/wacl-*.test         $(DEMOREPO)/tests/
	printf '%s\n' \
	    '<!doctype html>' \
	    '<html lang="en">' \
	    '<head>' \
	    '<meta charset="utf-8">' \
	    '<title>SurfTcl — Tcl in the browser</title>' \
	    '<meta http-equiv="refresh" content="0; url=wacl-minimal-demo/">' \
	    '<link rel="canonical" href="wacl-minimal-demo/">' \
	    '</head>' \
	    '<body>' \
	    '<p><a href="wacl-minimal-demo/">SurfTcl demo →</a></p>' \
	    '</body>' \
	    '</html>' > $(DEMOREPO)/index.html

clean:
	rm -f *.o surftcl.mjs surftcl.wasm \
	    wacl-minimal-demo/surftcl.mjs wacl-minimal-demo/surftcl.wasm \
	    wacl-minimal-demo/surftcl-bootstrap.mjs
	if [ -e tcl/unix/Makefile ] ; then cd tcl/unix && make clean ; fi

# We don't ever change the Tcl source tarball directly,
# so preserve it by default to save bandwidth.
distclean: clean
	rm -rf tcl

fullclean: distclean
	rm -f $(TCLSRC)
