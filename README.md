# fastboot-android

自己编译一份 **Android 静态 fastboot**：AOSP `platform/system/core` 的 fastboot 源码，
配 [lzhiyong/android-sdk-tools](https://github.com/lzhiyong/android-sdk-tools) 的 CMake 构建工程，
用 Android NDK 交叉编译成**不依赖任何共享库**的静态可执行文件，可以直接丢进手机
（`/data/local/tmp`、Termux、OZJFLASH 内置终端）里跑。

- 源码版本：**platform-tools 35.0.2**（AOSP tag `platform-tools-35.0.2`）
- 构建目标：`arm64-v8a` / `armeabi-v7a` / `x86_64`
- 链接方式：全静态（`-static`，`ANDROID_STL=c++_static`），链接时 strip

## 为什么自己编

官方 `platform-tools` 里的 Linux fastboot 是**动态链接 glibc** 的，Android 上跑不起来；
本项目把所有依赖（libusb / zlib / boringssl / pcre2 / selinux / AOSP 基础库）全部静态编进去，
产物只有一个文件，丢到手机上 `chmod 755` 就能用。

## 目录结构

```
fastboot-android/
├── CMakeLists.txt          # 顶层构建（只构建 fastboot 一个目标）
├── fastboot.cmake          # fastboot 可执行目标（来自上游 platform-tools/fastboot.cmake）
├── cmake/                  # 上游构建工程：静态链接设置 + 内置 zlib
├── lib/*.cmake             # AOSP 静态库定义（libbase / liblog / libcutils / libutils /
│                           #   libselinux / libsepol / libsparse / libziparchive /
│                           #   libbuildversion / libusb / libdiagnoseusb）
├── patches/misc/           # platform_tools_version.h（libbuildversion 需要）
├── src/                    # vendored AOSP / 第三方源码（见 sources.json）
├── build.sh                # 构建封装
└── .github/workflows/      # CI：NDK 编译 + 上传产物
```

## 源码清单

`src/` 下是 fastboot 需要的最小源码集，全部取自 AOSP tag `platform-tools-35.0.2`
（走清华大学 AOSP 镜像拉取，完整清单与仓库地址见 `sources.json`）：

| 目录 | AOSP 仓库 | 用途 |
| --- | --- | --- |
| `src/core` | platform/system/core | fastboot 本体、libsparse、libfs_mgr(liblp)、libcutils、libutils |
| `src/extras` | platform/system/extras | ext4_utils、f2fs_utils |
| `src/libbase` `src/logging` `src/libziparchive` | system/libbase、system/logging、system/libziparchive | 基础库 |
| `src/libusb` | platform/external/libusb | USB 通信（fastboot 协议载体） |
| `src/selinux` | platform/external/selinux | libselinux / libsepol（ext4_utils 的 file_contexts） |
| `src/boringssl` | platform/external/boringssl | crypto（哈希） |
| `src/pcre` | platform/external/pcre | pcre2-8（libselinux 正则） |
| `src/fmtlib` | platform/external/fmtlib | fmt（liblp 的格式化） |
| `src/avb` `src/mkbootimg` `src/soong` `src/googletest` | external/avb、system/tools/mkbootimg、build/soong、external/googletest | 头文件 / 版本号 |
| `src/zlib` | github.com/madler/zlib v1.3.1 | 内置静态 zlib |

## 构建

### 用 NDK（推荐，产物给手机用）

```bash
export ANDROID_NDK_HOME=/path/to/android-ndk-r27c   # 或 --ndk 指定
./build.sh                 # 默认 arm64-v8a / android-24 / 静态
./build.sh --arch arm      # armeabi-v7a
./build.sh --arch x86_64
```

产物：`build/ndk-static-<abi>/fastboot`

### 用本机 clang（调试/验证源码，产物是 Linux 可执行文件）

```bash
CC=clang CXX=clang++ ./build.sh --toolchain host --link shared
```

> 本机构建需要 `flex`（libsepol 的 `cil_lexer.l`）、`golang`（boringssl 代码生成）与 `perl`。
> 用 gcc 会失败：`libbase/logging.cpp` 用了 clang 内建的 `__builtin_available`。

### CI

GitHub Actions（`.github/workflows/build.yml`）在 push 时自动编译三个 ABI 并上传 artifact。

## 使用

```bash
adb push fastboot /data/local/tmp/fastboot
adb shell chmod 755 /data/local/tmp/fastboot
adb shell /data/local/tmp/fastboot devices
```

USB 访问需要有权限的进程（root 或者走 Android USB Host API 拿到 fd），
在 root shell / Termux 里直接运行即可；普通应用请配合 OZJFLASH 这类自带 USB 授权的容器使用。

## 许可

- 构建工程（CMake 文件）来自 [lzhiyong/android-sdk-tools](https://github.com/lzhiyong/android-sdk-tools)，Apache-2.0
- AOSP 源码遵循 AOSP 原始许可（Apache-2.0 / BSD 等，各仓库 LICENSE 随源码保留）
- 本项目 `LICENSE.txt` 为 Apache-2.0
