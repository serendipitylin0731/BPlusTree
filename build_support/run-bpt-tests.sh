#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-"$ROOT_DIR/build"}"
JOBS="${JOBS:-}"
RUN_TESTS=1
CLEAN=0

case "$BUILD_DIR" in
  /*) ;;
  *) BUILD_DIR="$ROOT_DIR/$BUILD_DIR" ;;
esac

TARGETS=(
  b_plus_tree_insert_test
  b_plus_tree_delete_test
  b_plus_tree_contention_test
  b_plus_tree_concurrent_test
)

usage() {
  cat <<EOF
Usage: bash build_support/run-bpt-tests.sh [--clean] [--no-run]

Options:
  --clean   Remove the build directory before configuring.
  --no-run  Build the B+ tree tests without running them.

Environment:
  BUILD_DIR  Build directory. Defaults to ./build.
  JOBS       Parallel build jobs. Defaults to detected CPU count.
EOF
}

detect_jobs() {
  if [[ -n "$JOBS" ]]; then
    printf "%s\n" "$JOBS"
  elif command -v nproc >/dev/null 2>&1; then
    nproc
  elif [[ "$(uname -s)" == "Darwin" ]]; then
    sysctl -n hw.ncpu
  else
    printf "2\n"
  fi
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --clean)
      CLEAN=1
      ;;
    --no-run)
      RUN_TESTS=0
      ;;
    -h | --help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
  shift
done

if ! command -v cmake >/dev/null 2>&1; then
  echo "cmake is required. On Ubuntu/WSL run: sudo bash build_support/packages.sh -y" >&2
  echo "On macOS run: bash build_support/packages.sh -y" >&2
  exit 1
fi

JOBS="$(detect_jobs)"
if ! [[ "$JOBS" =~ ^[0-9]+$ ]] || [[ "$JOBS" -lt 1 ]]; then
  JOBS=2
fi

case ":${ASAN_OPTIONS:-}:" in
  *:detect_leaks=*) ;;
  *) export ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}detect_leaks=0" ;;
esac

if [[ "$CLEAN" -eq 1 ]]; then
  rm -rf "$BUILD_DIR"
fi

mkdir -p "$BUILD_DIR"
(
  cd "$BUILD_DIR"
  cmake -DCMAKE_BUILD_TYPE=Debug "$ROOT_DIR"
)

for target in "${TARGETS[@]}"; do
  cmake --build "$BUILD_DIR" --target "$target" -- -j"$JOBS"
done

if [[ "$RUN_TESTS" -eq 0 ]]; then
  exit 0
fi

status=0
for target in "${TARGETS[@]}"; do
  test_binary="$BUILD_DIR/test/$target"
  if [[ ! -x "$test_binary" ]]; then
    echo "Missing executable: $test_binary" >&2
    status=1
    continue
  fi

  echo
  echo "==> Running $target"
  "$test_binary" || status=$?
done

exit "$status"
