/*
 * fdpass_recv.h — 免 root fd 传递「接收端」（工具二进制侧）。
 *
 * 与 xioo0317/fdpass（发送端 libfdpass.so）及 MOFLASH 魔改二进制的协议完全对齐：
 *   MOMIUI_USB_SOCK=@name  → connect 抽象 Unix socket → write 1B → recvmsg(SCM_RIGHTS)
 *   MOMIUI_USB_FD=<n>      → 同进程退化路径（直接把该数字当 fd 用）
 *
 * 纯 C，可被 C/C++ 混合工程直接链接；不依赖 JNI、不依赖任何第三方库。
 */
#ifndef FDPASS_RECV_H
#define FDPASS_RECV_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 环境变量名（与发送端 ToolConsole / 魔改二进制一致） */
#define FDPASS_ENV_SOCK "MOMIUI_USB_SOCK"
#define FDPASS_ENV_FD   "MOMIUI_USB_FD"

/* 返回码：>=0 为可用的 usbfs fd；<0 为下列错误 */
#define FDPASS_RECV_NO_ENV     (-100) /* 未设置注入环境变量 → 调用方应走原生路径 */
#define FDPASS_RECV_ERR_NAME   (-2)   /* MOMIUI_USB_SOCK 不以 '@' 开头或名字非法 */
#define FDPASS_RECV_ERR_SOCKET (-1)   /* socket() 失败 */
#define FDPASS_RECV_ERR_CONNECT (-3)  /* connect 重试后仍失败 */
#define FDPASS_RECV_ERR_WRITE  (-4)   /* 请求字节写入失败 */
#define FDPASS_RECV_ERR_RECVMSG (-5)  /* recvmsg 失败 */
#define FDPASS_RECV_ERR_NOFD   (-6)   /* 报文里没有 SCM_RIGHTS fd */
#define FDPASS_RECV_ERR_BADFD  (-7)   /* MOMIUI_USB_FD 不是有效 fd */

/**
 * 接收 App 侧 fdpass 传来的 usbfs fd。
 * 未注入时立即返回 FDPASS_RECV_NO_ENV（不重试、不阻塞），调用方据此回退原生扫描。
 */
int fdpass_recv_usbfs_fd(void);

/**
 * 由 fd 反查真实 devfs 路径（/dev/bus/usb/BBB/DDD），供 WaitForDisconnect 使用。
 * 反查失败时写入 /proc/self/fd/<n> 兜底，并返回 -1。
 */
int fdpass_devfs_path(int fd, char* out, size_t out_size);

/**
 * 由 fd 反查 sysfs 设备名（形如 "1-4"），供原生描述符解析读取 serial/interface。
 * 反查失败返回 -1（调用方用占位名，仅影响 serial 显示，不影响端点解析）。
 */
int fdpass_sysfs_name(int fd, char* out, size_t out_size);

#ifdef __cplusplus
}
#endif
#endif /* FDPASS_RECV_H */
