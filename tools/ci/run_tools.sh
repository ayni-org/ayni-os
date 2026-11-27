#!/usr/bin/env bash
# Owns the runner-local native tool image and its container invocation. The
# workflow owns commands and test policy; this script only supplies the tools.
set -euo pipefail
recipe_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
recipe="$recipe_dir/install_tools.sh"
base_image=ubuntu:24.04
recipe_key=$({ printf '%s\n' "$base_image"; cat "$recipe"; } | sha256sum | cut -d ' ' -f 1)
image="ayni-ci-tools:$recipe_key"
container_workspace=/__w/ayni/ayni
# Stay within the Docker host CPU budget on small runners (four vCPUs in
# public CI), while preserving the original ceiling on larger machines.
host_cpus=$(nproc)
container_cpus=$((host_cpus < 12 ? host_cpus : 12))
# Init forwards cancellation signals to the command and reaps child processes.
container_options=(--cpus="$container_cpus" --memory=3g --memory-swap=3g --init)
bootstrap_container=
cleanup() {
    if [ -n "$bootstrap_container" ]; then
        docker rm -f "$bootstrap_container" >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT
trap 'exit 130' INT TERM

prepare_image() {
    # One installer per recipe, including when separate runner instances later
    # share this Docker daemon. Failed/cancelled installs never publish an image.
    exec 9>"/tmp/ayni-ci-tools-$(id -u).lock"
    flock 9
    if ! docker image inspect "$image" >/dev/null 2>&1; then
        # Refresh the base only on a cold recipe; warm jobs need no registry.
        docker pull "$base_image" >&2
        bootstrap_container=$(docker create "${container_options[@]}" \
            --mount "type=bind,source=$recipe,target=/tmp/install_tools.sh,readonly" \
            "$base_image" bash /tmp/install_tools.sh)
        docker start -a "$bootstrap_container" >&2
        local status
        status=$(docker inspect --format '{{.State.ExitCode}}' "$bootstrap_container")
        if [ "$status" -ne 0 ]; then
            return "$status"
        fi
        docker commit --change 'CMD ["bash"]' "$bootstrap_container" "$image" >/dev/null
        docker rm "$bootstrap_container" >/dev/null
        bootstrap_container=
        echo "Created native tool image: $image" >&2
    else
        echo "Reusing native tool image: $image" >&2
    fi
}

run_tools() {
    # Preserve the former job-container path so debug ccache keys stay stable.
    # Run with the runner's identity; a cancelled step leaves no root-owned files.
    local workspace=${GITHUB_WORKSPACE:-$(git rev-parse --show-toplevel)}
    local relative_dir=${PWD#"$workspace"}
    if [ "$PWD" != "$workspace$relative_dir" ] || [[ "$relative_dir" != /* && -n "$relative_dir" ]]; then
        echo "Run tools from within the checkout: $workspace" >&2
        return 2
    fi
    local identity="$(id -u):$(id -g)"
    local args=(run --rm "${container_options[@]}" --pull=never --user "$identity"
        --mount "type=bind,source=$workspace,target=$container_workspace"
        --volume ayni-ccache:/ccache --volume ayni-downloads:/downloads
        --workdir "$container_workspace$relative_dir"
        --env GIT_CONFIG_GLOBAL=/dev/null)
    # Linked local worktrees refer to their common Git directory by absolute
    # path. A normal Actions checkout keeps .git inside the workspace instead.
    local git_common
    git_common=$(git rev-parse --path-format=absolute --git-common-dir)
    if [[ "$git_common" != "$workspace/"* ]]; then
        args+=(--mount "type=bind,source=$git_common,target=$git_common,readonly")
    fi
    local variable
    for variable in CC CXX CMAKE_BUILD_PARALLEL_LEVEL CTEST_PARALLEL_LEVEL \
        CMAKE_C_COMPILER_LAUNCHER CMAKE_CXX_COMPILER_LAUNCHER \
        CCACHE_DIR CCACHE_MAXSIZE CCACHE_SLOPPINESS \
        GIT_CONFIG_COUNT GIT_CONFIG_KEY_0 GIT_CONFIG_VALUE_0; do
        if [[ -v "$variable" ]]; then
            args+=(--env "$variable")
        fi
    done
    exec docker "${args[@]}" "$image" "$@"
}

case "${1:-}" in
    prepare)
        prepare_image
        # Named caches predate this image: adopt them only from root, as the
        # former job containers and a new volume leave them. Runners sharing
        # this Docker daemon must run as one user; otherwise each job would
        # take the caches from a job of the other runner still using them.
        docker run --rm "${container_options[@]}" --pull=never \
            --volume ayni-ccache:/ccache --volume ayni-downloads:/downloads \
            "$image" bash -eu -c '
                for cache in /ccache /downloads; do
                    owner=$(stat -c %u:%g "$cache")
                    if [ "$owner" = 0:0 ]; then
                        chown -R "$1" "$cache"
                    elif [ "$owner" != "$1" ]; then
                        echo "$cache belongs to $owner, not $1: run every runner on this Docker daemon as one user" >&2
                        exit 1
                    fi
                done' bash "$(id -u):$(id -g)"
        ;;
    mmap-bits)
        # This host sysctl can be root-readable only. Keep this read separate
        # from user-owned build commands, with no checkout or cache mounts.
        docker run --rm "${container_options[@]}" --pull=never \
            "$image" cat /proc/sys/vm/mmap_rnd_bits
        ;;
    run)
        shift
        run_tools "$@"
        ;;
    *) echo "Usage: $0 {prepare|mmap-bits|run COMMAND [ARG ...]}" >&2; exit 2 ;;
esac
