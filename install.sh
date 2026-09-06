#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
prefix=${PREFIX:-"$HOME/.local"}

for dependency in make cc curl-config; do
    if ! command -v "$dependency" >/dev/null 2>&1; then
        printf 'gsh install: missing required command: %s\n' "$dependency" >&2
        exit 1
    fi
done

exec make -C "$repo_dir" install PREFIX="$prefix"
