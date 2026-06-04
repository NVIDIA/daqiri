#!/usr/bin/env bash
set -euo pipefail

# Remove DAQIRI artifacts (CMake install and/or container image) while leaving
# every build prerequisite (DPDK, DOCA libraries, CUDA, hugepages, NIC drivers,
# kernel modules, apt state) untouched. Mirrors the conventions of
# scripts/build-container.sh: env-var configuration with sensible defaults.

DAQIRI_PREFIX="${DAQIRI_PREFIX:-/opt/daqiri}"
IMAGE_TAG="${IMAGE_TAG:-daqiri:local}"
BUILD_DIR="${BUILD_DIR:-build}"

ASSUME_YES=0
DRY_RUN=0
CLEAN_BUILD=0
TARGET=""

if [[ -t 1 ]]; then
    C_RED=$'\033[31m'; C_YELLOW=$'\033[33m'; C_GREEN=$'\033[32m'
    C_CYAN=$'\033[36m'; C_DIM=$'\033[2m'; C_RESET=$'\033[0m'
else
    C_RED=""; C_YELLOW=""; C_GREEN=""; C_CYAN=""; C_DIM=""; C_RESET=""
fi

info() { printf '%s[info]%s %s\n'  "${C_CYAN}"   "${C_RESET}" "$*"; }
warn() { printf '%s[warn]%s %s\n'  "${C_YELLOW}" "${C_RESET}" "$*" >&2; }
fail() { printf '%s[error]%s %s\n' "${C_RED}"    "${C_RESET}" "$*" >&2; }

usage() {
    cat <<EOF
Usage: $(basename "$0") <target> [options]

Targets:
  cmake       Remove the bare-metal CMake install at \$DAQIRI_PREFIX
  container   Remove the DAQIRI container image and any containers spawned from it
  all         Both of the above

Options:
  -y, --yes          Skip confirmation prompts
  -n, --dry-run      Print actions without executing
      --clean-build  For 'cmake' target, also remove \$BUILD_DIR
  -h, --help         Show this help

Environment variables (with current defaults):
  DAQIRI_PREFIX=${DAQIRI_PREFIX}
  IMAGE_TAG=${IMAGE_TAG}
  BUILD_DIR=${BUILD_DIR}

Removes DAQIRI artifacts only. Build prerequisites (DPDK, DOCA libraries,
CUDA, hugepages, NIC drivers) are preserved.

Exit codes:
  0  success (all verification checks passed)
  1  one or more verification checks failed
  2  argument or usage error
EOF
}

if [[ $# -lt 1 ]]; then
    usage >&2
    exit 2
fi

case "$1" in
    cmake|container|all) TARGET="$1"; shift ;;
    -h|--help)           usage; exit 0 ;;
    *)
        fail "unknown target '$1'"
        usage >&2
        exit 2
        ;;
esac

while [[ $# -gt 0 ]]; do
    case "$1" in
        -y|--yes)      ASSUME_YES=1 ;;
        -n|--dry-run)  DRY_RUN=1 ;;
        --clean-build) CLEAN_BUILD=1 ;;
        -h|--help)     usage; exit 0 ;;
        *)
            fail "unknown option '$1'"
            usage >&2
            exit 2
            ;;
    esac
    shift
done

run() {
    if [[ $DRY_RUN -eq 1 ]]; then
        printf '%s[dry-run]%s %s\n' "${C_DIM}" "${C_RESET}" "$*"
    else
        # shellcheck disable=SC2294
        # eval is intentional here: callers pass pre-quoted command strings
        # that include shell redirection (e.g. "sudo xargs rm < '$manifest'")
        # which has to be re-parsed by the shell.
        eval "$@"
    fi
}

confirm() {
    [[ $ASSUME_YES -eq 1 || $DRY_RUN -eq 1 ]] && return 0
    local reply
    printf '%s [y/N]: ' "$1"
    read -r reply
    [[ "$reply" =~ ^[Yy]$ ]]
}

remove_cmake_via_manifest() {
    local manifest="$1"
    local count
    count=$(wc -l < "$manifest")
    info "Manifest: $manifest ($count files)"
    printf '%sPreview (first 5 entries):%s\n' "${C_DIM}" "${C_RESET}"
    head -5 "$manifest" | sed 's/^/  /'

    confirm "Remove these $count files (sudo required)?" || {
        warn "Aborted by user."
        return 1
    }

    # xargs -d '\n' (not the default whitespace splitting) so an install
    # prefix containing spaces does not get carved up into wrong rm
    # arguments. CMake writes one absolute path per line in
    # install_manifest.txt, so newline is the only safe delimiter.
    run "sudo xargs -d '\\n' rm -v < '$manifest'"

    info "Removing now-empty directories under $DAQIRI_PREFIX"
    run "sudo find '$DAQIRI_PREFIX' -depth -type d -empty -delete 2>/dev/null || true"
}

remove_cmake_via_scan() {
    info "Scanning $DAQIRI_PREFIX for DAQIRI-owned artifacts."
    local -a candidates=()
    [[ -e "$DAQIRI_PREFIX/include/daqiri"          ]] && candidates+=("$DAQIRI_PREFIX/include/daqiri")
    [[ -e "$DAQIRI_PREFIX/lib/cmake/daqiri"        ]] && candidates+=("$DAQIRI_PREFIX/lib/cmake/daqiri")
    [[ -e "$DAQIRI_PREFIX/lib/pkgconfig/daqiri.pc" ]] && candidates+=("$DAQIRI_PREFIX/lib/pkgconfig/daqiri.pc")

    if [[ -d "$DAQIRI_PREFIX/lib" ]]; then
        while IFS= read -r -d '' f; do candidates+=("$f"); done \
            < <(find "$DAQIRI_PREFIX/lib" -maxdepth 1 -name 'libdaqiri*' -print0 2>/dev/null)
    fi
    if [[ -d "$DAQIRI_PREFIX/bin" ]]; then
        while IFS= read -r -d '' f; do candidates+=("$f"); done \
            < <(find "$DAQIRI_PREFIX/bin" -maxdepth 1 \
                \( -name 'daqiri_bench_*' -o -name 'daqiri_example_*' \
                   -o -name 'daqiri_bench_*.yaml' -o -name 'tune_system.py' \) \
                -print0 2>/dev/null)
    fi

    # Vendored entries are flagged for manual review only; their names collide
    # with system installs at shared prefixes, so the script will not remove
    # them without an explicit human decision.
    local -a manual=()
    [[ -e "$DAQIRI_PREFIX/include/spdlog"   ]] && manual+=("$DAQIRI_PREFIX/include/spdlog")
    [[ -e "$DAQIRI_PREFIX/include/yaml-cpp" ]] && manual+=("$DAQIRI_PREFIX/include/yaml-cpp")
    if [[ -d "$DAQIRI_PREFIX/lib" ]]; then
        while IFS= read -r -d '' f; do manual+=("$f"); done \
            < <(find "$DAQIRI_PREFIX/lib" -maxdepth 1 -name 'libyaml-cpp*' -print0 2>/dev/null)
    fi

    if [[ ${#candidates[@]} -eq 0 && ${#manual[@]} -eq 0 ]]; then
        info "Nothing found under $DAQIRI_PREFIX."
        return 0
    fi

    if [[ ${#candidates[@]} -gt 0 ]]; then
        info "DAQIRI-owned artifacts (will be removed):"
        printf '  %s\n' "${candidates[@]}"
    fi
    if [[ ${#manual[@]} -gt 0 ]]; then
        # Both header and listing go to stderr so they stay grouped under
        # piping/redirection (warn() writes to stderr).
        warn "Vendored third-party artifacts (manual review, not removed):"
        printf '  %s\n' "${manual[@]}" >&2
        warn "These names can collide with system installs. Remove them yourself if appropriate."
    fi

    [[ ${#candidates[@]} -eq 0 ]] && return 0

    confirm "Remove the DAQIRI-owned artifacts above (sudo required)?" || {
        warn "Aborted by user."
        return 1
    }

    local entry
    for entry in "${candidates[@]}"; do
        run "sudo rm -rfv '$entry'"
    done

    info "Removing now-empty directories under $DAQIRI_PREFIX"
    run "sudo find '$DAQIRI_PREFIX' -depth -type d -empty -delete 2>/dev/null || true"
}

action_cmake() {
    info "Target: cmake (prefix=$DAQIRI_PREFIX, build=$BUILD_DIR)"
    local removed=0
    if [[ ! -e "$DAQIRI_PREFIX" ]]; then
        info "$DAQIRI_PREFIX does not exist; nothing to remove."
    else
        local manifest="$BUILD_DIR/install_manifest.txt"
        if [[ -f "$manifest" ]]; then
            remove_cmake_via_manifest "$manifest" || return 1
        else
            warn "$manifest not found. Falling back to a prefix scan."
            warn "To use the manifest-driven path, regenerate it with"
            warn "  cmake --install $BUILD_DIR --prefix $DAQIRI_PREFIX"
            remove_cmake_via_scan || return 1
        fi
        removed=1
    fi

    # Refresh the linker cache once, after either removal path. Without
    # this, verify_cmake's `ldconfig -p | grep daqiri` check trips a
    # spurious WARN on hosts that registered the install prefix in
    # /etc/ld.so.conf.d/. Skipped when nothing was removed so the script
    # stays a no-op on a clean system.
    if [[ $removed -eq 1 ]]; then
        info "Refreshing dynamic linker cache."
        run "sudo ldconfig"
    fi

    if [[ $CLEAN_BUILD -eq 1 ]]; then
        if [[ -d "$BUILD_DIR" ]]; then
            confirm "Also remove the build directory $BUILD_DIR?" && run "rm -rf '$BUILD_DIR'"
        else
            info "Build directory $BUILD_DIR not present."
        fi
    fi

    verify_cmake
}

# Note: BuildKit cache (`docker builder prune`) is intentionally not touched.
# The cache is shared across every project on the host and cannot be filtered
# by image reference, so pruning it would affect unrelated builds. Developers
# who need that space back should run `docker builder prune` manually.
action_container() {
    info "Target: container (image=$IMAGE_TAG)"
    if ! command -v docker >/dev/null 2>&1; then
        warn "docker not found on PATH; skipping container removal."
        return 0
    fi

    local containers
    containers=$(docker ps -a --filter "ancestor=$IMAGE_TAG" --format '{{.ID}}' 2>/dev/null || true)
    if [[ -n "$containers" ]]; then
        info "Containers spawned from $IMAGE_TAG:"
        docker ps -a --filter "ancestor=$IMAGE_TAG" \
            --format '  {{.ID}}  {{.Names}}  ({{.Status}})'
        if confirm "Stop and remove these containers?"; then
            run "docker rm -f $(echo "$containers" | tr '\n' ' ')"
        fi
    else
        info "No containers found for $IMAGE_TAG."
    fi

    if docker image inspect "$IMAGE_TAG" >/dev/null 2>&1; then
        confirm "Remove image $IMAGE_TAG?" && run "docker image rm '$IMAGE_TAG'"
    else
        info "Image $IMAGE_TAG not present."
    fi

    verify_container
}

verify_cmake() {
    if [[ $DRY_RUN -eq 1 ]]; then
        info "Verification skipped (dry run)."
        return 0
    fi
    info "Verifying CMake cleanup:"
    local rc=0

    # Check for DAQIRI-specific artifacts under the prefix. Vendored
    # spdlog/yaml-cpp files may legitimately remain after a fallback-scan run
    # and are reported separately as informational.
    local -a daqiri_remnants=()
    [[ -e "$DAQIRI_PREFIX/include/daqiri"          ]] && daqiri_remnants+=("$DAQIRI_PREFIX/include/daqiri")
    [[ -e "$DAQIRI_PREFIX/lib/cmake/daqiri"        ]] && daqiri_remnants+=("$DAQIRI_PREFIX/lib/cmake/daqiri")
    [[ -e "$DAQIRI_PREFIX/lib/pkgconfig/daqiri.pc" ]] && daqiri_remnants+=("$DAQIRI_PREFIX/lib/pkgconfig/daqiri.pc")
    if [[ -d "$DAQIRI_PREFIX/lib" ]]; then
        while IFS= read -r -d '' f; do daqiri_remnants+=("$f"); done \
            < <(find "$DAQIRI_PREFIX/lib" -maxdepth 1 -name 'libdaqiri*' -print0 2>/dev/null)
    fi

    if [[ ${#daqiri_remnants[@]} -eq 0 ]]; then
        printf '  %sOK%s    no DAQIRI artifacts remain under %s\n' \
            "${C_GREEN}" "${C_RESET}" "$DAQIRI_PREFIX"
    else
        printf '  %sFAIL%s  %d DAQIRI artifact(s) still present under %s:\n' \
            "${C_RED}" "${C_RESET}" "${#daqiri_remnants[@]}" "$DAQIRI_PREFIX"
        printf '         %s\n' "${daqiri_remnants[@]}"
        rc=1
    fi

    if pkg-config --modversion daqiri >/dev/null 2>&1; then
        printf '  %sFAIL%s  pkg-config still resolves daqiri\n' "${C_RED}" "${C_RESET}"; rc=1
    else
        printf '  %sOK%s    pkg-config cannot find daqiri\n' "${C_GREEN}" "${C_RESET}"
    fi

    if ldconfig -p 2>/dev/null | grep -qi daqiri; then
        printf '  %sWARN%s  ldconfig cache still references daqiri (run: sudo ldconfig)\n' "${C_YELLOW}" "${C_RESET}"
    else
        printf '  %sOK%s    ldconfig cache clean\n' "${C_GREEN}" "${C_RESET}"
    fi

    # Informational note if the prefix dir still exists with non-DAQIRI content
    # (typically vendored spdlog / yaml-cpp left over from a fallback-scan run).
    if [[ -d "$DAQIRI_PREFIX" ]] && [[ ${#daqiri_remnants[@]} -eq 0 ]]; then
        local leftover_count
        leftover_count=$(find "$DAQIRI_PREFIX" -mindepth 1 2>/dev/null | wc -l)
        if [[ $leftover_count -gt 0 ]]; then
            printf '  %sINFO%s  %s still exists (%d non-DAQIRI item(s) remain, e.g. vendored spdlog / yaml-cpp)\n' \
                "${C_CYAN}" "${C_RESET}" "$DAQIRI_PREFIX" "$leftover_count"
        fi
    fi

    return $rc
}

verify_container() {
    if [[ $DRY_RUN -eq 1 ]]; then
        info "Verification skipped (dry run)."
        return 0
    fi
    info "Verifying container cleanup:"
    if ! command -v docker >/dev/null 2>&1; then
        printf '  %sOK%s    docker not installed; nothing to verify\n' "${C_GREEN}" "${C_RESET}"
        return 0
    fi
    if docker images --format '{{.Repository}}:{{.Tag}}' | grep -q "^${IMAGE_TAG}$"; then
        printf '  %sFAIL%s  %s still present\n' "${C_RED}" "${C_RESET}" "$IMAGE_TAG"
        return 1
    fi
    printf '  %sOK%s    %s absent\n' "${C_GREEN}" "${C_RESET}" "$IMAGE_TAG"

    # Surface partial-build leftovers (e.g. an interrupted `docker build` of
    # this image leaves dangling intermediates and an exited build container,
    # neither of which carries the $IMAGE_TAG reference). These are not removed
    # automatically because they can also legitimately come from unrelated
    # builds on the same host; flag them so the user can decide.
    local dangling_count exited_count
    dangling_count=$(docker images -q --filter dangling=true 2>/dev/null | wc -l)
    exited_count=$(docker ps -aq --filter status=exited 2>/dev/null | wc -l)
    if [[ $dangling_count -gt 0 || $exited_count -gt 0 ]]; then
        printf '  %sINFO%s  %d dangling image(s) and %d exited container(s) remain\n' \
            "${C_CYAN}" "${C_RESET}" "$dangling_count" "$exited_count"
        printf '         (likely from an interrupted build of %s; reclaim with:\n' "$IMAGE_TAG"
        printf '          docker container prune -f && docker image prune -f)\n'
    fi
}

overall_status=0
case "$TARGET" in
    cmake)     action_cmake     || overall_status=$? ;;
    container) action_container || overall_status=$? ;;
    all)
        action_cmake     || overall_status=$?
        action_container || overall_status=$?
        ;;
esac

[[ $DRY_RUN -eq 1 ]] && info "Dry run complete. No changes were made."

exit "$overall_status"
