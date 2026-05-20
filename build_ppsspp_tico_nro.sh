#!/bin/bash

set -euo pipefail

export DEVKITPRO=/opt/devkitpro
export DEVKITARM=$DEVKITPRO/devkitARM
export DEVKITPPC=$DEVKITPRO/devkitPPC
export DEVKITA64=$DEVKITPRO/devkitA64

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build_switch_tico"
MESA_NVK_DIR="${MESA_NVK_DIR:-/nvk-build}"

if [ -z "${SWITCH_VULKAN_LIBRARY:-}" ] && [ -f "${MESA_NVK_DIR}/src/nouveau/vulkan/libvulkan.a" ]; then
	SWITCH_VULKAN_LIBRARY="${MESA_NVK_DIR}/src/nouveau/vulkan/libvulkan.a"
fi

echo "=== Building PPSSPP Tico NRO ==="
echo "Source: ${SCRIPT_DIR}"
echo "Build:  ${BUILD_DIR}"
if [ -n "${SWITCH_VULKAN_LIBRARY:-}" ]; then
	echo "NVK:    ${SWITCH_VULKAN_LIBRARY}"
else
	echo "NVK:    devkitPro default"
fi
echo ""

if [ "${1:-}" = "clean" ]; then
	echo "Cleaning build directory..."
	rm -rf "${BUILD_DIR}"
fi

mkdir -p "${BUILD_DIR}"

cmake_args=(
	-S "${SCRIPT_DIR}"
	-B "${BUILD_DIR}"
	-DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake"
	-DUSE_LIBNX=ON
	-DSWITCH_VULKAN_ONLY=ON
	-DUSE_DISCORD=OFF
	-DHEADLESS=ON
	-DLIBRETRO=OFF
	-DCMAKE_BUILD_TYPE=Release
	-U CMAKE_EXE_LINKER_FLAGS_RELEASE
	-DCMAKE_C_FLAGS_RELEASE="-O3 -DNDEBUG -g0 -ffunction-sections -fdata-sections -fomit-frame-pointer"
	-DCMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG -g0 -ffunction-sections -fdata-sections -fomit-frame-pointer"
)

if [ -n "${SWITCH_VULKAN_LIBRARY:-}" ]; then
	cmake_args+=(-U SWITCH_VULKAN_LIBRARY -DSWITCH_VULKAN_LIBRARY:FILEPATH="$SWITCH_VULKAN_LIBRARY")
fi

cmake "${cmake_args[@]}"

cmake --build "${BUILD_DIR}" -j"$(nproc)" --target tico-ppsspp_nro

if [ -f "${BUILD_DIR}/tico-ppsspp.nro" ]; then
	echo "======================================"
	echo "Build successful!"
	echo "Output: ${BUILD_DIR}/tico-ppsspp.nro"
	echo "======================================"
else
	echo "Error: tico-ppsspp.nro not found"
	exit 1
fi
