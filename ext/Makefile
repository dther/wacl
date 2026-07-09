# ext/ — the package build pipeline.
#
# Turns first-party package source under ../packages/ into per-package
# release zips under build/. A zip is the distribution unit: mount it
# with `tcl::zipfs::mount` (or, once the JS bridge exposes it,
# `surftcl::mount`) and add the mountpoint to auto_path. This mirrors
# the zipfs packaging philosophy in CLAUDE.md — one artifact per
# package, no resolver, the release page is the index.
#
# Layout of an artifact: wacl-json.zip contains
#   wacl-json/pkgIndex.tcl
#   wacl-json/wacl-json.tcl
# i.e. the package directory sits at the zip root, so a consumer mounts
# the zip at any point M and `lappend auto_path M` finds the package in
# its wacl-json/ subdirectory — exactly how a plain on-disk auto_path
# directory works.
#
# Vendored tcllib modules will land here too, sliced on tcllib's own
# module boundaries (each subdir already ships a pkgIndex.tcl). That
# machinery is identical to the per-package rule below; it's just a
# matter of unpacking the tcllib tarball and pointing the same zip step
# at modules/<name>/. Not wired yet — see CLAUDE.md's punted list.

PKGDIR = ../packages
BUILD  = build

# Every wacl-* directory under ../packages/ is a buildable package.
PKGS = $(notdir $(wildcard $(PKGDIR)/wacl-*))

.PHONY: all packages clean
all: packages

packages: $(PKGS:%=$(BUILD)/%.zip)

# One rule per package, generated so each zip depends on exactly the
# files in its source directory (edit any of them and the zip rebuilds).
# zip is run from inside PKGDIR so the archive entries are
# `wacl-<name>/...` rather than `../packages/wacl-<name>/...`; the output
# path is made absolute because the recipe has already cd'd away.
define PKG_RULE
$(BUILD)/$(1).zip: $(wildcard $(PKGDIR)/$(1)/*)
	@mkdir -p $(BUILD)
	@rm -f $$@
	cd $(PKGDIR) && zip -rq "$(CURDIR)/$$@" $(1)
endef
$(foreach p,$(PKGS),$(eval $(call PKG_RULE,$(p))))

clean:
	rm -rf $(BUILD)
