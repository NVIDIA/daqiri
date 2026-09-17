#!/usr/bin/env bash
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

PYTHON="${PYTHON:-python3}"
VENV="${VENV:-.venv}"

if [ ! -x "${VENV}/bin/python" ]; then
  "${PYTHON}" -m venv "${VENV}"
fi

"${VENV}/bin/python" -m pip install --requirement tests/requirements.txt
"${VENV}/bin/python" -m pytest tests/portable
