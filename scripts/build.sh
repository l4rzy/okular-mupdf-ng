#!/usr/bin/env bash
set -e

BUILD_DIR="build"
BUILD_TYPE="Debug"
RUN_TESTS=1
WARNING_FLAGS="-Wall -Wextra -Wpedantic -Wconversion -Wshadow"

case "$1" in
  release)
    BUILD_DIR="build-release"
    BUILD_TYPE="Release"
    RUN_TESTS=0
    ;;
  asan)
    BUILD_DIR="build-asan"
    BUILD_TYPE="Debug"
    RUN_TESTS=1
    ;;
  clean)
    echo "Cleaning build directories..."
    rm -rf build build-release build-asan
    exit 0
    ;;
  format)
    echo "Formatting source code with clang-format..."
    find src tests \( -name "*.cpp" -o -name "*.hpp" \) | xargs clang-format -i
    echo "Formatting complete."
    exit 0
    ;;
  dev|test|"")
    BUILD_DIR="build"
    BUILD_TYPE="Debug"
    RUN_TESTS=1
    ;;
  *)
    echo "Usage: $0 [dev|release|asan|test|format|clean]"
    exit 1
    ;;
esac

if ! compgen -G 'thirdparty/mupdf*/Makefile' > /dev/null; then
  ./scripts/download-mupdf.sh
fi

echo "==> Configuring ($BUILD_TYPE) in $BUILD_DIR..."
cmake_args=(
-G Ninja
-DCMAKE_BUILD_TYPE="$BUILD_TYPE"
-DCMAKE_C_COMPILER=clang
-DCMAKE_CXX_COMPILER=clang++
"-DCMAKE_C_FLAGS=$WARNING_FLAGS"
"-DCMAKE_CXX_FLAGS=$WARNING_FLAGS"
)
if [ "$BUILD_TYPE" = "Release" ]; then
  cmake_args+=(-DBUILD_TESTING=OFF)
fi
if [ "$1" = "asan" ]; then
  cmake_args+=(-DENABLE_SANITIZERS=ON)
fi
cmake -B "$BUILD_DIR" -S . "${cmake_args[@]}"

echo "==> Building ($BUILD_TYPE)..."
cmake --build "$BUILD_DIR" -j$(nproc)

if [ "$RUN_TESTS" -eq 1 ]; then
  echo "==> Running tests..."
  ctest --test-dir "$BUILD_DIR" --output-on-failure
fi

echo "==> Build finished successfully."
