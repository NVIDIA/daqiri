PYTHON ?= python3
REPO_ROOT := $(shell git -C "$(CURDIR)" rev-parse --show-toplevel 2>/dev/null)
GENERATED_PATHS := \
	docs/images/packet_diagrams/hds \
	docs/images/packet_diagrams/flow_steering \
	docs/images/packet_diagrams/reorder \
	docs/images/packet_diagrams/reorder_quantize

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
	@if [ -n "$$(git -C "$(REPO_ROOT)" status --porcelain -- $(GENERATED_PATHS))" ]; then \
		git -C "$(REPO_ROOT)" status --short -- $(GENERATED_PATHS); \
		echo "error: packet diagram outputs are stale; rerun make -C docs/images/packet_diagrams and commit the results" >&2; \
		exit 1; \
	fi
	@echo "Packet diagram outputs are reproducible."

clean:
	$(RM) hds/*.webp hds/*-poster.png
	$(RM) flow_steering/*.webp flow_steering/*-poster.png
	$(RM) reorder/*.webp reorder/*-poster.png
	$(RM) reorder_quantize/*.webp reorder_quantize/*-poster.png
