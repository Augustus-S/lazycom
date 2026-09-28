lazycom_root := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)
outdir ?= $(lazycom_root)/build/srpm
spec ?= packaging/lazycom.spec

.PHONY: srpm
srpm:
	@if ! command -v git >/dev/null 2>&1; then \
		if [ "$$(id -u)" -ne 0 ]; then \
			echo "SRPM generation requires Git; install git-core first." >&2; \
			exit 1; \
		fi; \
		dnf -y install git-core || exit $$?; \
	fi
	cd "$(lazycom_root)" && bash packaging/build-srpm.sh "$(outdir)" "$(spec)"
