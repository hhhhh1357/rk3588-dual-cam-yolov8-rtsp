#!/bin/bash
#
# Cross-compile v4l2_yolov_dual_mpp for the RK3588 board (aarch64 buildroot).
#
# Usage:
#   ./build.sh                      # uses the default toolchain path
#   TOOLCHAIN_DIR=/opt/xxx ./build.sh   # point at another aarch64 toolchain
#
# Output goes to ./install/v4l2_yolov_dual_mpp

set -e

TARGET_ARCH=aarch64
TOOLCHAIN_DIR="${TOOLCHAIN_DIR:-/opt/atk-dlrk3588-toolchain}"

export CC="${TOOLCHAIN_DIR}/bin/aarch64-buildroot-linux-gnu-gcc"
export CXX="${TOOLCHAIN_DIR}/bin/aarch64-buildroot-linux-gnu-g++"

ROOT_PWD=$(cd "$(dirname "$0")" && pwd)
BUILD_DIR="${ROOT_PWD}/build"
INSTALL_DIR="${ROOT_PWD}/install"

echo "TOOLCHAIN_DIR = ${TOOLCHAIN_DIR}"
echo "CXX           = ${CXX}"

rm -rf "${BUILD_DIR}" "${INSTALL_DIR}"
mkdir -p "${BUILD_DIR}"

cd "${BUILD_DIR}"
cmake .. \
    -DCMAKE_SYSTEM_NAME=Linux \
    -DCMAKE_SYSTEM_PROCESSOR=${TARGET_ARCH} \
    -DCMAKE_BUILD_TYPE=Release \
    -DTOOLCHAIN_DIR=${TOOLCHAIN_DIR} \
    -DCMAKE_INSTALL_PREFIX=${INSTALL_DIR}
make -j"$(nproc)"
make install

echo ""
echo "Build OK: ${INSTALL_DIR}/v4l2_yolov_dual_mpp"
