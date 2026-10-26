#!/usr/bin/env bash
# Build ASCII Player on Linux / WSL without CMake.
#
#   ./build-linux.sh             dynamic binary
#   STATIC=1 ./build-linux.sh    fully static x86_64 binary (recommended for release)
#   OUT=... ./build-linux.sh     custom output path
set -euo pipefail
cd "$(dirname "$0")"

CXX="${CXX:-}"
if [[ -z "$CXX" ]]; then
    if command -v g++ >/dev/null 2>&1; then
        CXX=g++
    elif command -v clang++ >/dev/null 2>&1; then
        CXX=clang++
    else
        echo "No C++ compiler found. Install g++ or clang." >&2
        exit 1
    fi
fi
OUT="${OUT:-build-linux/ascii-player}"
STATIC="${STATIC:-0}"
if [[ "${1:-}" == "--static" ]]; then
    STATIC=1
fi

EXTRA=()
if [[ "$STATIC" == "1" ]]; then
    EXTRA+=(-static -static-libstdc++ -static-libgcc)
fi

mkdir -p "$(dirname "$OUT")"

# shellcheck disable=SC2086
"$CXX" -std=c++17 -O2 -pthread -Wall -Wextra \
    ${CXXFLAGS:-} \
    src/audio.cpp \
    src/common.cpp \
    src/json.cpp \
    src/main.cpp \
    src/media.cpp \
    src/process.cpp \
    src/render.cpp \
    "${EXTRA[@]}" \
    -o "$OUT"

echo "Built: $OUT"
file "$OUT" 2>/dev/null || true
