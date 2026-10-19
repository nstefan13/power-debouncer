#!/bin/sh
# Builds powerfilter for arm64 Android and packs the flashable Magisk module.
#
# Run it ONE of these ways:
#   a) In Termux on the phone:   pkg install clang zip && sh build.sh
#   b) On a PC with the NDK:     NDK=/path/to/android-ndk sh build.sh
#
# Result: powerfilter-magisk.zip  (flash it in the Magisk app > Modules)
set -e
cd "$(dirname "$0")"

if [ -n "$CC" ]; then
  :                                    # user supplied a compiler
elif [ -n "$NDK" ]; then
  CC=$(ls "$NDK"/toolchains/llvm/prebuilt/*/bin/aarch64-linux-android30-clang | head -n1)
elif command -v clang >/dev/null 2>&1 && [ -n "$TERMUX_VERSION" -o -d /data/data/com.termux ]; then
  CC=clang                             # Termux's clang already targets Android arm64
else
  echo "No compiler found. Set NDK=/path/to/ndk (PC) or run inside Termux." >&2
  exit 1
fi

echo "Compiler: $CC"
mkdir -p module/bin
"$CC" -O2 powerfilter.c -o module/bin/powerfilter
echo "Built module/bin/powerfilter"

rm -f powerfilter-magisk.zip
( cd module && zip -r9 ../powerfilter-magisk.zip . )
echo "Done: $(pwd)/powerfilter-magisk.zip"
