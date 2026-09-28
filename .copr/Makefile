lazycom_root := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)
outdir ?= $(lazycom_root)/build/srpm
spec ?= packaging/lazycom.spec

.PHONY: srpm
srpm:
	cd "$(lazycom_root)" && bash packaging/build-srpm.sh "$(outdir)" "$(spec)"
