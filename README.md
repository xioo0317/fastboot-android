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

## 产物

| ABI | 体积 | 说明 |
| --- | --- | --- |
| `arm64-v8a` | 1.5 MB | 主流机型 |
| `armeabi-v7a` | 1.0 MB | 老 32 位机型 |
| `x86_64` | 1.6 MB | 模拟器 / x86 设备 |

已验证（NDK r27c，arm64-v8a）：

```
$ file fastboot
fastboot: ELF 64-bit LSB executable, ARM aarch64, version 1 (SYSV), statically linked, stripped

$ llvm-readelf -d fastboot | grep NEEDED
(无输出 —— 不依赖任何共享库)

$ qemu-aarch64 ./fastboot --version
fastboot version 35.0.2-
```

CI 每次 push 都会重编三个 ABI 并上传 artifact。

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

## 免 root：fdpass 接收端（FB 传递）

本仓库的 fastboot 现已内置 **fd 传递接收端**，与自有实现
[`xioo0317/fdpass`](https://github.com/xioo0317/fdpass)（App 侧 `libfdpass.so`）协议一致，
不再需要 MOFLASH 的魔改二进制。

### USB 后端归属（重要）

| 二进制 | USB 后端 | fd 注入点 |
| --- | --- | --- |
| MOFLASH 魔改 `libfastboot.so` | **libusb 后端**（`src/libusb/libusb/os/linux_usbfs.c`） | `get_usbfs_fd()`：把 `open("/dev/bus/usb/%03u/%03u")` 换成收 fd，再走 `/proc/self/fd/<fd>` 反查 + `detach_kernel_driver_and_claim` |
| **本仓库标准版 fastboot** | **AOSP 原生 usbfs 后端** `src/core/fastboot/usb_linux.cpp`（`USBDEVFS_*` ioctl + `/dev/bus/usb/%03d/%03d`） | `find_usb_device()` 开头 |

注意：仓库里虽然有 `src/libusb/libusb` 和 `lib/libusb.cmake`，但那个 `libusb` 静态库
**没有链接进 `fastboot` 目标**（`fastboot.cmake` 的 `target_link_libraries` 里没有它），
且仓库不含 `usb_libusb.cpp`。即 `src/libusb/libusb` 在当前构建中是**死代码**，
改它不会生效；标准版真正编译进可执行文件的 USB 后端只有 `usb_linux.cpp`。
两套后端只是「怎么打开设备」不同，fd 传递的协议层（`MOMIUI_USB_*` + `SCM_RIGHTS`）完全相同。

**两端分工**

| 端 | 位置 | 职责 |
| --- | --- | --- |
| 发送端 | App 侧 `libfdpass.so`（`xioo0317/fdpass`） | `serveFdLoop(fd, "@name")` 起抽象 Unix socket，把已授权的 usbfs fd 用 `SCM_RIGHTS` 发出 |
| 接收端 | **本仓库** `fdpass/fdpass_recv.c` | `connect` 抽象 socket → `recvmsg(SCM_RIGHTS)` 取 fd，交给原生 `usb_linux.cpp` 做 USBDEVFS 传输 |

**运行环境**（由 App 侧注入，与 OZJFLASH `ToolConsole` 一致）

```bash
MOMIUI_USB_SOCK=@momiui_fb_<nanoTime>   # fastboot 每条命令随机名
MOMIUI_USB_FD=<fd>                      # 同进程退化路径（跨进程无效，仅兼容保留）
```

**行为约定**

- **未设置上述环境变量时，原生 sysfs 扫描路径 100% 不变**（root / PC / Termux 行为完全一致，零开销，也不打印任何诊断）。
- 设置时：`find_usb_device()` 优先接收 fd → **先 `lseek(fd,0,SEEK_SET)` 复位共享文件偏移**再 `read`
  usbfs 描述符（`SCM_RIGHTS` 传的是同一 open file description，App 侧可能已读过导致偏移非 0）；
  read 不足时从 `/sys/bus/usb/devices/<name>/descriptors` 兜底（对齐 libusb 后端做法）
  → 复用原生 `filter_usb_device()` 解析 `ep_in/ep_out/ifc` → `USBDEVFS_DISCONNECT_CLAIM`
  自己 detach+claim（App 侧不 claim，避免 EBUSY）→ 用 `/proc/self/fd/N` 反查真实 devfs 路径供
  `WaitForDisconnect`。
- 反查失败时退化为占位 sysfs 名，只影响 serial 显示，不影响端点解析与刷写。
- `connect` 失败重试 `101 × 20ms`（约 2s），与魔改二进制一致，容忍 App 侧服务稍晚启动。
- **socket 通道失败时回退 `MOMIUI_USB_FD`**（对齐魔改 so 的 `0x298ba8` 分支）：App 侧
  `getFileDescriptor()` 的 usbfs fd 通常不带 `CLOEXEC`，`ProcessBuilder` fork+exec 后 fd 号不变，
  子进程可直接沿用。真机日志「socket 未在 1.5s 内就绪，但有 FB=5」正是这一路径。
- 全过程带 `[fdpass]` 前缀诊断输出到 stderr（App 的 `redirectErrorStream(true)` 会捕获），
  依次报告 connect、SCM_RIGHTS fd、`/proc/self/fd` 反查、描述符来源、filter 结果、
  claim 结果，便于定位 `< waiting for any device >`。

**协议契约**（与 `xioo0317/fdpass::fdpass_core.c` 逐字对齐）

| 项 | 值 |
| --- | --- |
| 地址 | `sockaddr_un` 抽象命名空间：`sun_path[0]=0`，`@` 后名字复制到 `sun_path+1`，`addrlen=strlen+3` |
| 报文 | 1 字节数据 `0x66` + `SCM_RIGHTS`，`cmsg_len=20` / `msg_controllen=24` |
| 请求 | 接收端先 `write` 1 字节触发发送端的 `poll(2s)` |

**本地验证**（不需要 NDK / Android 设备）

```bash
cc -std=c11 -O2 -Wall -Wextra -Werror -pthread -I fdpass \
   tests/fdpass_interop_test.c fdpass/fdpass_recv.c -o /tmp/fdpass_interop_test
/tmp/fdpass_interop_test      # ALL PASS
```

该测试内置最小发送端（复刻 `serveFdLoop` 协议），用 pipe 验证收到的 fd 指向同一内核对象；
另覆盖「无环境变量」「非法 socket 名」「`MOMIUI_USB_FD` 退化」「socket 失败回退 FD」
「socket 失败且无 FD」五条分支。CI 的 `fdpass-interop` job 每次 push 都会跑一遍，再编译三个 ABI。

也可用自有发送端源码做真实交叉互通：

```bash
cc -std=c11 -O2 -Wall -Wextra -Werror -pthread \
   -I fdpass -I /path/to/fdpass/src \
   /path/to/fdpass/src/fdpass_core.c fdpass/fdpass_recv.c <驱动 main> -o /tmp/xcheck
```

### 排障：`< waiting for any device >`

该提示来自 `fastboot.cpp::open_device()` 的轮询，含义是 `usb_open()` 没拿到 handle。按 `[fdpass]`
诊断行逐段判断：

| 现象 | 原因 | 处理 |
| --- | --- | --- |
| 只有 `connect ... 超时` | App 侧 `serveFdLoop` 没起来 / `waitSocketReady` 1.5s 超时 | 确认 `libfdpass.so` 已换为自有源码编译版、`FdPass` 包名 `com.Android.MoFlash.data` 与 JNI 符号一致；工具侧重试 2s 会自动兜底 |
| `退化使用 MOMIUI_USB_FD=N → 非 usbfs` | 跨 exec 后该 fd 号不是 usbfs | 以 socket 通道为主；检查 App 是否提前关闭了 `UsbDeviceConnection` |
| `usbfs read 仅 0 字节` | App 侧已读过描述符，共享偏移非 0 | 已由 `lseek(0)` + sysfs 兜底自动处理 |
| `filter_usb_device rc=-1` | VID/PID 或接口非 `FF/42/03`（可能设备不在 fastboot 模式） | 用 `UsbFdTracer.interfaces()` 确认接口类别 |
| 完全没有 `[fdpass]` 行 | 环境变量没注入到工具进程 | 检查 `ProcessBuilder.environment()` |

## 许可

- 构建工程（CMake 文件）来自 [lzhiyong/android-sdk-tools](https://github.com/lzhiyong/android-sdk-tools)，Apache-2.0
- AOSP 源码遵循 AOSP 原始许可（Apache-2.0 / BSD 等，各仓库 LICENSE 随源码保留）
- 本项目 `LICENSE.txt` 为 Apache-2.0
