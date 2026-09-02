cmake_minimum_required(VERSION 3.10)
project(rk3588_dual_cam_yolov8_rtsp C CXX)

# ---------- compile options ----------
set(CMAKE_CXX_STANDARD 11)
add_definitions(-O2 -g -Wno-write-strings -Wno-return-type)

# ---------- toolchain / sysroot ----------
# Override with:  cmake .. -DTOOLCHAIN_DIR=/path/to/aarch64-buildroot-toolchain
set(TOOLCHAIN_DIR /opt/atk-dlrk3588-toolchain CACHE PATH
    "RK3588 aarch64 cross toolchain root (contains aarch64-buildroot-linux-gnu/sysroot)")
set(SYSROOT ${TOOLCHAIN_DIR}/aarch64-buildroot-linux-gnu/sysroot)
set(SYSROOT_INC ${SYSROOT}/usr/include)
set(SYSROOT_LIB ${SYSROOT}/usr/lib)

# ---------- system include dirs (all from the buildroot sysroot) ----------
include_directories(
    ${SYSROOT_INC}/gstreamer-1.0
    ${SYSROOT_INC}/glib-2.0
    ${SYSROOT_LIB}/glib-2.0/include   # generated glib headers
)

# ---------- project include dirs ----------
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/src       # main program + result_socket
    ${CMAKE_CURRENT_SOURCE_DIR}/support   # vendored rknn_model_zoo support code
    ${SYSROOT_INC}/rga
    ${SYSROOT_INC}/rockchip               # MPP headers
    ${SYSROOT_INC}/opencv4
    ${SYSROOT_INC}/rknn                   # librknnrt headers
)

# ---------- sources ----------
# src/      : self-written application code (see LICENSE)
# support/  : Apache-2.0 code vendored from Rockchip rknn_model_zoo
add_executable(v4l2_yolov_dual_mpp
    src/v4l2_yolov_dual_mpp.cpp
    src/result_socket.cc
    support/postprocess.cc
    support/yolov8.cc
    support/file_utils.c
)

# ---------- link libs (resolved against the cross toolchain sysroot) ----------
target_link_libraries(v4l2_yolov_dual_mpp
    ${SYSROOT_LIB}/librknnrt.so
    rockchip_mpp       # external H.265 (HEVC) encoder
    rga                # 2D accel / format convert
    opencv_core
    opencv_imgproc
    gstreamer-1.0
    gstrtspserver-1.0
    gstapp-1.0
    gobject-2.0
    glib-2.0
    pthread
)

# ---------- install ----------
install(TARGETS v4l2_yolov_dual_mpp DESTINATION .)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/model/yolov8.rknn DESTINATION model)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/scripts/result_receiver_demo.py DESTINATION scripts)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/scripts/rtsp_probe_client.py DESTINATION scripts)
