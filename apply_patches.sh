#!/usr/bin/env bash
# Apply every patch in gitPatches/ to its matching submodule.
# Naming: ext_<name>.patch -> ext/<name>, <name>.patch -> <name>
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PATCH_DIR="$ROOT/gitPatches"
failed=0

for patch in "$PATCH_DIR"/*.patch; do
    name="$(basename "$patch" .patch)"
    case "$name" in
        ext_*) module="ext/${name#ext_}" ;;
        *)     module="$name" ;;
    esac

    dir="$ROOT/$module"
    if [ ! -d "$dir" ]; then
        echo "[skip]    $name: module '$module' not found"
        failed=1
        continue
    fi

    if git -C "$dir" apply --reverse --check "$patch" >/dev/null 2>&1; then
        echo "[ok]      $name: already applied to $module"
    elif git -C "$dir" apply --check "$patch" >/dev/null 2>&1; then
        git -C "$dir" apply "$patch"
        echo "[applied] $name -> $module"
    else
        echo "[error]   $name: does not apply cleanly to $module"
        failed=1
    fi
done

exit "$failed"
