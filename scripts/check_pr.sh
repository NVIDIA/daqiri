#!/usr/bin/env bash
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

run_docker_base=0
run_diagrams=0
PYTHON="${PYTHON:-python3}"
VENV="${VENV:-.venv}"

usage() {
  cat <<'USAGE'
Usage: scripts/check_pr.sh [--diagrams] [--docker-base]

Runs the standard local checks before opening a PR:
  - portable Python tests
  - documentation build and documentation reference checks

Options:
  --diagrams    Regenerate packet diagram assets before checking docs.
  --docker-base  Also build the Docker base stage.
  -h, --help     Show this help.
USAGE
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --docker-base)
      run_docker_base=1
      ;;
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

if [ ! -x "${VENV}/bin/python" ]; then
  "${PYTHON}" -m venv "${VENV}"
fi

VENV="$(cd "${VENV}" && pwd -P)"
PYTHON="${VENV}/bin/python"
export PYTHON VENV

scripts/check_portable_tests.sh

docs_args=()
if [ "${run_diagrams}" -eq 1 ]; then
  docs_args+=(--diagrams)
fi

scripts/check_docs.sh "${docs_args[@]}"

if [ "${run_docker_base}" -eq 1 ]; then
  scripts/check_docker_base.sh
fi
