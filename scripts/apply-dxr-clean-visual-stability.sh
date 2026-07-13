#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"
apply_if_needed() {
  local patch="$1"
  if git apply --check "$patch" >/dev/null 2>&1; then
    git apply --whitespace=nowarn "$patch"
    echo "Applied patch: $patch"
    return 0
  fi
  if git apply --reverse --check "$patch" >/dev/null 2>&1; then
    echo "Patch already applied, skipping: $patch"
    return 0
  fi
  echo "Patch cannot be applied cleanly: $patch" >&2
  git apply --check "$patch" || true
  exit 1
}
apply_if_needed patches/00-build-fix-Com_Printf.patch
apply_if_needed patches/02-dxr-stable-mode.patch
apply_if_needed patches/03-pvs-decompressvis-x64.patch
apply_if_needed patches/04-dxr-visibility-debug.patch
apply_if_needed patches/05-dxr-clean-visual-stability-only.patch
