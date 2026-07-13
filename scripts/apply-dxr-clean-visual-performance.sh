#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
for p in \
  patches/00-build-fix-Com_Printf.patch \
  patches/02-dxr-stable-mode.patch \
  patches/03-pvs-decompressvis-x64.patch \
  patches/04-dxr-visibility-debug.patch \
  patches/05-dxr-clean-visual-stability-only.patch \
  patches/06-dxr-clean-visual-performance-pipeline.patch; do
  if git apply --check "$p"; then
    git apply --whitespace=nowarn "$p"
    echo "Applied $p"
  elif git apply --reverse --check "$p"; then
    echo "Already applied: $p"
  else
    echo "Cannot apply $p exactly. On Windows/GitHub Actions use scripts/apply-dxr-clean-visual-performance.ps1, which has validated fallback transformations." >&2
    exit 1
  fi
done
