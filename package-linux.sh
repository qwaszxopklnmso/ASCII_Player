#!/usr/bin/env bash
# Package a self-contained Linux release directory and tar.gz.
set -euo pipefail
cd "$(dirname "$0")"

NAME="ASCIIPlayer-linux-x86_64"
ROOT="dist-linux/$NAME"
ARCHIVE="dist-linux/$NAME.tar.gz"

rm -rf "$ROOT" "$ARCHIVE"
mkdir -p "$ROOT"

OUT="$ROOT/ascii-player" STATIC=1 ./build-linux.sh
if command -v strip >/dev/null 2>&1; then
    strip --strip-all "$ROOT/ascii-player" 2>/dev/null || true
fi

cp README.md LICENSE "$ROOT/"
if [[ -d samples ]]; then cp -r samples "$ROOT/"; fi
if [[ -d docs ]]; then cp -r docs "$ROOT/"; fi

if [[ -n "${BUNDLED_FFMPEG:-}" ]]; then
    cp "$BUNDLED_FFMPEG" "$ROOT/ffmpeg"
    chmod +x "$ROOT/ffmpeg"
    if [[ -n "${BUNDLED_FFMPEG_LICENSE:-}" ]]; then
        cp "$BUNDLED_FFMPEG_LICENSE" "$ROOT/FFMPEG_LICENSE.txt"
    fi
    echo "Bundled FFmpeg: $BUNDLED_FFMPEG"
else
    echo "No Linux FFmpeg bundled; the release expects 'ffmpeg' from PATH."
fi

tar --mtime='UTC 2020-01-01' --owner=0 --group=0 --numeric-owner \
    -C dist-linux -czf "$ARCHIVE" "$NAME"

echo
echo "Packaged: $ARCHIVE"
ls -lh "$ARCHIVE" "$ROOT/ascii-player"
if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$ARCHIVE" "$ROOT/ascii-player"
fi
