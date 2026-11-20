PYTHON ?= $(shell sh -c 'for c in python3.12 python3; do $$c -c "from PIL import features; import sys; sys.exit(0 if features.check(\"webp\") else 1)" >/dev/null 2>&1 && { echo $$c; exit 0; }; done; echo python3')
REPO_ROOT := $(shell git -C "$(CURDIR)" rev-parse --show-toplevel 2>/dev/null)
ANIMATIONS := \
	hds/header-data-split.webp \
	hds/header-data-split.gif \
	flow_steering/flow-steering.webp \
	flow_steering/flow-steering.gif \
	reorder/packet-reorder.webp \
	reorder/packet-reorder.gif \
	reorder_quantize/packet-reorder-quantize.webp \
	reorder_quantize/packet-reorder-quantize.gif
POSTERS := \
	hds/header-data-split-poster.png \
	flow_steering/flow-steering-poster.png \
	reorder/packet-reorder-poster.png \
	reorder_quantize/packet-reorder-quantize-poster.png

.PHONY: all hds flow-steering reorder check clean

all: hds flow-steering reorder

hds:
	$(PYTHON) hds_animation.py

flow-steering:
	$(PYTHON) flow_steering_animation.py

reorder:
	$(PYTHON) reorder_animation.py

check: all
	@if [ -z "$(REPO_ROOT)" ]; then \
		echo "error: packet diagram reproducibility check requires a git checkout" >&2; \
		exit 1; \
	fi
	@missing=0; \
	for f in $(ANIMATIONS); do \
		if [ ! -f "$(CURDIR)/$$f" ]; then \
			echo "error: missing generated animation $$f" >&2; \
			missing=1; \
		fi; \
	done; \
	if [ $$missing -ne 0 ]; then exit 1; fi
	@if ! git -C "$(REPO_ROOT)" diff --quiet -- $(addprefix docs/images/packet_diagrams/,$(POSTERS)); then \
		git -C "$(REPO_ROOT)" diff --name-status -- $(addprefix docs/images/packet_diagrams/,$(POSTERS)); \
		echo "error: poster PNGs are stale; rerun make -C docs/images/packet_diagrams, git add the posters, and commit" >&2; \
		exit 1; \
	fi
	@echo "Packet diagram outputs are reproducible."

clean:
	$(RM) hds/*.webp hds/*.gif
	$(RM) flow_steering/*.webp flow_steering/*.gif
	$(RM) reorder/*.webp reorder/*.gif
	$(RM) reorder_quantize/*.webp reorder_quantize/*.gif
	$(RM) $(POSTERS)
