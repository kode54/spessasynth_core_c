#!/usr/bin/env bash
# Builds the C library (into a harness-private build dir, so the repo's own
# build/ is left alone) and links the offline renderer against it.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CORE_SRC="$HERE/../spessasynth_core"
BUILD_DIR="$HERE/build"
CORE_BUILD="$BUILD_DIR/core"
BUILD_TYPE="${BUILD_TYPE:-RelWithDebInfo}"

mkdir -p "$BUILD_DIR"

if [ ! -f "$CORE_BUILD/CMakeCache.txt" ]; then
    GEN=()
    command -v ninja >/dev/null 2>&1 && GEN=(-G Ninja)
    cmake -S "$CORE_SRC" -B "$CORE_BUILD" "${GEN[@]}" \
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
        -DSS_BUILD_EXAMPLES=OFF \
        -DSS_BUILD_SHARED=ON
fi

cmake --build "$CORE_BUILD" --target spessasynth --parallel

# Locate the freshly built shared library.
LIB=""
for candidate in "$CORE_BUILD"/libspessasynth.dylib "$CORE_BUILD"/libspessasynth.so "$CORE_BUILD"/spessasynth.dll "$CORE_BUILD"/libspessasynth.a; do
    [ -f "$candidate" ] && LIB="$candidate" && break
done
if [ -z "$LIB" ]; then
    echo "Could not find the built spessasynth library under $CORE_BUILD" >&2
    exit 1
fi

CC="${CC:-cc}"
EXTRA_LIBS=(-lm)
case "$(uname -s)" in
    Darwin) RPATH=(-Wl,-rpath,"$CORE_BUILD") ;;
    *)      RPATH=(-Wl,-rpath,"$CORE_BUILD") ;;
esac

"$CC" -O2 -g -std=c11 -Wall -Wextra \
    -I"$CORE_SRC/include" \
    -o "$BUILD_DIR/ss_render_c" \
    "$HERE/render_c.c" \
    "$LIB" "${RPATH[@]}" "${EXTRA_LIBS[@]}"

"$CC" -O2 -g -std=c11 -Wall -Wextra \
    -I"$CORE_SRC/include" \
    -o "$BUILD_DIR/callback_lead_in_test" \
    "$HERE/tools/callback_lead_in_test.c" \
    "$LIB" "${RPATH[@]}" "${EXTRA_LIBS[@]}"

echo "Built $BUILD_DIR/ss_render_c against $LIB"
echo "Built $BUILD_DIR/callback_lead_in_test"
