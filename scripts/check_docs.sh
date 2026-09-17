#!/usr/bin/env bash
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

PYTHON="${PYTHON:-python3}"
run_diagrams=0
required_diagrams=(
  docs/images/packet_diagrams/hds/header-data-split.webp
  docs/images/packet_diagrams/flow_steering/flow-steering.webp
  docs/images/packet_diagrams/reorder/packet-reorder.webp
  docs/images/packet_diagrams/reorder_quantize/packet-reorder-quantize.webp
)

usage() {
  cat <<'USAGE'
Usage: scripts/check_docs.sh [--diagrams]

Builds and validates the documentation.

Options:
  --diagrams  Force regeneration of packet diagram assets before building.
  -h, --help  Show this help.
USAGE
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --diagrams)
      run_diagrams=1
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "ERROR: unknown argument '$1'" >&2
      usage >&2
      exit 1
      ;;
  esac
  shift
done

"${PYTHON}" -m pip install mkdocs-material pillow
if [ "${run_diagrams}" -eq 0 ]; then
  for diagram in "${required_diagrams[@]}"; do
    if [ ! -f "${diagram}" ]; then
      echo "Missing ${diagram}; generating packet diagram assets."
      run_diagrams=1
      break
    fi
  done
fi

if [ "${run_diagrams}" -eq 1 ]; then
  make -C docs/images/packet_diagrams PYTHON="${PYTHON}" all
fi
"${PYTHON}" -m mkdocs build --strict
"${PYTHON}" scripts/check_html_links.py site
"${PYTHON}" scripts/check_doc_refs.py
