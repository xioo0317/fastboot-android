#!/usr/bin/env bash
#
# 构建 Android 静态 fastboot
#
#   ./build.sh                      # 默认 NDK / arm64-v8a / android-24
#   ./build.sh --arch arm           # armeabi-v7a
#   ./build.sh --arch x86_64
#   ./build.sh --toolchain host     # 用本机编译器构建（快速验证源码）
#   ./build.sh --ndk /path/to/ndk
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

TOOLCHAIN=ndk
ARCH=arm64
API=30   # android-base/unique_fd.h 用到 fdsan，上游要求最低 API 30
LINK=static
BUILD_TYPE=Release
NDK="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-${ANDROID_NDK:-}}}"
JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --toolchain) TOOLCHAIN="$2"; shift 2;;
        --arch)      ARCH="$2"; shift 2;;
        --api)       API="$2"; shift 2;;
        --link)      LINK="$2"; shift 2;;
        --ndk)       NDK="$2"; shift 2;;
        --jobs)      JOBS="$2"; shift 2;;
        --debug)     BUILD_TYPE=Debug; shift;;
        -h|--help)   sed -n '2,14p' "$0"; exit 0;;
        *) echo "unknown option: $1" >&2; exit 1;;
    esac
done

case "$ARCH" in
    arm64|aarch64)   ABI=arm64-v8a;;
    arm|armv7|armeabi-v7a) ABI=armeabi-v7a;;
    x86_64|amd64)    ABI=x86_64;;
    x86|i686)        ABI=x86;;
    *) echo "unsupported arch: $ARCH" >&2; exit 1;;
esac

STATIC=ON
[[ "$LINK" == "shared" ]] && STATIC=OFF

BUILD_DIR="build/${TOOLCHAIN}-${LINK}-${ABI}"
ARGS=(
    -G Ninja
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5
    -DSDK_STATIC="$STATIC"
    -DSDK_STRIP=ON
)

if [[ "$TOOLCHAIN" == "ndk" ]]; then
    if [[ -z "$NDK" || ! -f "$NDK/build/cmake/android.toolchain.cmake" ]]; then
        echo "error: NDK not found, set --ndk or ANDROID_NDK_HOME" >&2
        exit 1
    fi
    ARGS+=(
        -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake"
        -DANDROID_NDK="$NDK"
        -DANDROID_ABI="$ABI"
        -DANDROID_PLATFORM="android-$API"
        -DANDROID_STL=c++_static
    )
elif [[ "$TOOLCHAIN" == "host" ]]; then
    : # 使用本机默认编译器
else
    echo "unsupported toolchain: $TOOLCHAIN" >&2
    exit 1
fi

echo "==> configure: ${TOOLCHAIN} / ${ABI} / android-${API} / ${LINK} link"
cmake -B "$BUILD_DIR" "${ARGS[@]}"

echo "==> build"
cmake --build "$BUILD_DIR" --parallel "$JOBS"

OUT="$BUILD_DIR/fastboot"
if [[ -f "$OUT" ]]; then
    echo "==> output: $OUT"
    file "$OUT" 2>/dev/null || true
    ls -lh "$OUT"
else
    echo "error: $OUT not found" >&2
    exit 1
fi
