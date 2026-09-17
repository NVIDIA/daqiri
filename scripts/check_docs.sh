#!/usr/bin/env bash
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

PYTHON="${PYTHON:-python3}"

"${PYTHON}" -m pip install mkdocs-material pillow
make -C docs/images/packet_diagrams PYTHON="${PYTHON}" all
"${PYTHON}" -m mkdocs build --strict
"${PYTHON}" scripts/check_html_links.py site
"${PYTHON}" scripts/check_doc_refs.py
