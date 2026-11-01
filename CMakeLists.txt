#
# fastboot for Android —— 自包含构建（AOSP platform-tools 35.0.2 源码 + lzhiyong/android-sdk-tools 构建工程）
#
# 只构建 fastboot 一个可执行目标，静态链接（SDK_STATIC=ON），
# 产物是可以在 adb shell / Termux 里直接跑的静态 PIE 可执行文件。
#
# 用法见 build.sh：
#   ./build.sh --arch arm64      # NDK 交叉编译 arm64-v8a
#   ./build.sh --toolchain host  # 用本机编译器构建（调试 / 验证用）
#

cmake_minimum_required(VERSION 3.19)

# 部分第三方工程声明的 cmake_minimum_required < 3.5，CMake 4.x 需要策略下限
if(NOT DEFINED CMAKE_POLICY_VERSION_MINIMUM)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "")
endif()

project(fastboot C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

list(PREPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake/modules)

# platform tools version，从 patches/misc/platform_tools_version.h 读取
file(STRINGS ${PROJECT_SOURCE_DIR}/patches/misc/platform_tools_version.h _tools_version_line
    REGEX "^#define[ \t]+PLATFORM_TOOLS_VERSION[ \t]+\"[^\"]+\"")
string(REGEX REPLACE "^.*\"([^\"]+)\".*$" "\\1" TOOLS_VERSION "${_tools_version_line}")
if(NOT TOOLS_VERSION)
    message(FATAL_ERROR "can't read PLATFORM_TOOLS_VERSION from patches/misc/platform_tools_version.h")
endif()

set(SRC ${PROJECT_SOURCE_DIR}/src)

# 64-bit off_t for lseek
add_definitions(-D_FILE_OFFSET_BITS=64)

# bundled zlib must exist before any find_package(ZLIB) of the third-party projects
include(cmake/zlib.cmake)
# toolchain independent settings (static link, system libraries ...)
include(cmake/platform.cmake)

message(STATUS "fastboot version: ${TOOLS_VERSION}")
message(STATUS "target: ${SDK_TARGET_OS}-${CMAKE_SYSTEM_PROCESSOR}, compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "static link: ${SDK_STATIC}, strip: ${SDK_STRIP}")

# ---------------------------------------------------------------------------
# third-party libraries
#   fmtlib     -> fmt::fmt      (libfs_mgr 的日志格式化)
#   pcre2      -> pcre2-8       (libselinux 的 file_contexts 正则)
#   boringssl  -> crypto        (fastboot / liblp 的哈希)
# ---------------------------------------------------------------------------
if(NOT TARGET fmt::fmt)
    set(FMT_TEST OFF CACHE BOOL "" FORCE)
    set(FMT_DOC OFF CACHE BOOL "" FORCE)
    set(FMT_INSTALL OFF CACHE BOOL "" FORCE)
    add_subdirectory(src/fmtlib EXCLUDE_FROM_ALL)
endif()

if(NOT TARGET pcre2-8)
    # 只要 8 位宽那一份，16/32 位与 pcre2grep 都关掉
    set(PCRE2_BUILD_PCRE2_16 OFF CACHE BOOL "" FORCE)
    set(PCRE2_BUILD_PCRE2_32 OFF CACHE BOOL "" FORCE)
    set(PCRE2_BUILD_PCRE2GREP OFF CACHE BOOL "" FORCE)
    set(PCRE2_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(PCRE2_SUPPORT_JIT OFF CACHE BOOL "" FORCE)
    add_subdirectory(src/pcre EXCLUDE_FROM_ALL)
endif()

if(NOT TARGET crypto)
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    add_subdirectory(src/boringssl EXCLUDE_FROM_ALL)
endif()

# ---------------------------------------------------------------------------
# AOSP static libraries (lib/*.cmake 与 lzhiyong/android-sdk-tools 保持一致)
# ---------------------------------------------------------------------------
add_subdirectory(lib)

# ---------------------------------------------------------------------------
# fastboot
# ---------------------------------------------------------------------------
include(fastboot.cmake)
