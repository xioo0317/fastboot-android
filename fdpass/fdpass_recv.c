/*
 * fdpass_recv.c — 免 root fd 传递「接收端」实现。
 *
 * 协议逐条对齐发送端（xioo0317/fdpass :: fdpass_core.c）与 MOFLASH 魔改二进制：
 *   - env MOMIUI_USB_SOCK = "@momiui_fb_<t>"（抽象 Unix socket，'@' 不进 sun_path）
 *   - socket(AF_UNIX, SOCK_STREAM) → connect（失败重试 101×20ms，与魔改二进制一致）
 *   - write 1 字节请求 → recvmsg 收到 1 字节数据(0x66) + SCM_RIGHTS 携带的 usbfs fd
 *   - env MOMIUI_USB_FD = "<fd>" 为同进程退化路径（跨进程无效，仅保留兼容）
 *
 * 设计约束：
 *   1) 未注入时立即返回 FDPASS_RECV_NO_ENV，绝不阻塞、绝不改变原生扫描行为；
 *   2) 纯 POSIX，只用 libc（socket/readlink/opendir/read），静态链接友好；
 *   3) 收到的 fd 归属本进程，由调用方在 Close() 时关闭。
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "fdpass_recv.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define ABSTRACT_PREFIX   '@'
#define SUN_PATH_COPY_MAX 106          /* sizeof(sockaddr_un.sun_path) - 2 */
#define REQ_BYTE          0x66         /* 发送端恒发 'f'；写什么不重要，只需 1 字节触发 */
#define CONNECT_RETRIES   101
#define CONNECT_RETRY_MS  20
#define RECV_TIMEOUT_MS   3000

/*
 * 注入模式诊断：App 侧 ProcessBuilder 已 redirectErrorStream(true)，
 * 这些 [fdpass] 行会出现在工具终端，用于真机定位「有 FB 却没设备」。
 * 只在设置了 MOMIUI_USB_* 时才输出，未注入时原生行为保持 100% 静默。
 */
static int g_diag_enabled = -1;
static void diag(const char* fmt, ...) {
    if (g_diag_enabled < 0) {
        g_diag_enabled = (getenv(FDPASS_ENV_SOCK) != NULL || getenv(FDPASS_ENV_FD) != NULL);
    }
    if (!g_diag_enabled) return;
    va_list ap;
    va_start(ap, fmt);
    fputs("[fdpass] ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    fflush(stderr);
    va_end(ap);
}

/* 构造 Linux 抽象 socket 地址；name 必须以 '@' 开头 */
static int make_addr(const char* name, struct sockaddr_un* out, socklen_t* out_len) {
    if (name == NULL || name[0] != ABSTRACT_PREFIX) return FDPASS_RECV_ERR_NAME;
    const char* tail = name + 1;
    size_t n = strlen(tail);
    if (n == 0 || n > SUN_PATH_COPY_MAX) return FDPASS_RECV_ERR_NAME;

    memset(out, 0, sizeof(*out));
    out->sun_family = AF_UNIX;
    out->sun_path[0] = '\0';                 /* 抽象命名空间 */
    memcpy(out->sun_path + 1, tail, n);      /* 末尾由 memset 保证有 NUL */
    *out_len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + n);
    return 0;
}

static void nap_ms(long ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) { /* 被打断则续睡 */ }
}

/* 重试次数：默认 101（对齐魔改 so 0x298aa4 的 mov w19,#0x65）。
 * 允许 FDPASS_CONNECT_RETRIES 覆盖，仅供 host 互通测试缩短等待，真机不设置。 */
static int connect_retry_limit(void) {
    const char* s = getenv("FDPASS_CONNECT_RETRIES");
    if (s == NULL || *s == '\0') return CONNECT_RETRIES;
    char* end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0' || v < 1 || v > 10000) return CONNECT_RETRIES;
    return (int)v;
}

/* connect 抽象 socket；服务端可能晚于本进程启动，需重试 */
static int connect_abstract(const char* name) {
    struct sockaddr_un addr;
    socklen_t addrlen;
    int rc = make_addr(name, &addr, &addrlen);
    if (rc != 0) return rc;
    const int retries = connect_retry_limit();

    for (int attempt = 0; attempt < retries; ++attempt) {
        int s = socket(AF_UNIX, SOCK_STREAM, 0);
        if (s < 0) return FDPASS_RECV_ERR_SOCKET;
        if (connect(s, (const struct sockaddr*)&addr, addrlen) == 0) {
            diag("connect %s OK (attempt %d)", name, attempt + 1);
            return s;
        }
        int e = errno;
        close(s);
        /* 对齐魔改 libfastboot.so 0x298ab0 处的 errno 白名单：
         * ENOENT(2) / ECONNREFUSED(111) / ECONNRESET(104) 可重试，其余立即失败。
         * ECONNRESET 是「服务端 accept 前被关闭」的常见表现，原实现漏了它。 */
        if (e != ECONNREFUSED && e != ENOENT && e != EAGAIN && e != ECONNRESET) {
            diag("connect %s 硬错误 errno=%d (%s)", name, e, strerror(e));
            return FDPASS_RECV_ERR_CONNECT;
        }
        nap_ms(CONNECT_RETRY_MS);
    }
    diag("connect %s 超时：重试 %d×%dms 后服务端仍未 listen",
         name, retries, CONNECT_RETRY_MS);
    return FDPASS_RECV_ERR_CONNECT;
}

/* write 1 字节请求 → recvmsg 取 SCM_RIGHTS fd */
static int receive_fd(int sock) {
    char req = (char)REQ_BYTE;
    if (write(sock, &req, 1) != 1) {
        diag("write 请求字节失败 errno=%d (%s)", errno, strerror(errno));
        return FDPASS_RECV_ERR_WRITE;
    }

    struct pollfd pfd = { .fd = sock, .events = POLLIN, .revents = 0 };
    int pr = poll(&pfd, 1, RECV_TIMEOUT_MS);
    if (pr <= 0) {
        diag("等待服务端 sendmsg 超时/出错 pr=%d errno=%d", pr, errno);
        return FDPASS_RECV_ERR_RECVMSG;
    }

    char data;
    struct iovec iov = { .iov_base = &data, .iov_len = 1 };
    union {
        struct cmsghdr align;
        char buf[CMSG_SPACE(sizeof(int))];
    } control;
    memset(&control, 0, sizeof(control));

    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control.buf;
    msg.msg_controllen = sizeof(control.buf);

    ssize_t n = recvmsg(sock, &msg, 0);
    if (n < 0) {
        diag("recvmsg 失败 errno=%d (%s)", errno, strerror(errno));
        return FDPASS_RECV_ERR_RECVMSG;
    }
    if (msg.msg_flags & MSG_CTRUNC) {
        diag("recvmsg 控制报文被截断 (MSG_CTRUNC)，controllen 不足");
        return FDPASS_RECV_ERR_NOFD;
    }

    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg == NULL || cmsg->cmsg_level != SOL_SOCKET ||
        cmsg->cmsg_type != SCM_RIGHTS || cmsg->cmsg_len < CMSG_LEN(sizeof(int))) {
        diag("recvmsg 未携带 SCM_RIGHTS (cmsg=%p)", (void*)cmsg);
        return FDPASS_RECV_ERR_NOFD;
    }
    int fd;
    memcpy(&fd, CMSG_DATA(cmsg), sizeof(fd));
    if (fd < 0 || fcntl(fd, F_GETFD) == -1) {
        diag("SCM_RIGHTS 收到的 fd=%d 无效", fd);
        return FDPASS_RECV_ERR_BADFD;
    }
    char link[64], target[PATH_MAX + 1];
    snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    ssize_t rn = readlink(link, target, sizeof(target) - 1);
    if (rn > 0) {
        target[rn] = '\0';
        diag("SCM_RIGHTS 收到 fd=%d → %s%s", fd, target,
             strncmp(target, "/dev/bus/usb/", 13) == 0 ? " [usbfs OK]" : " [非 usbfs?]");
    } else {
        diag("SCM_RIGHTS 收到 fd=%d", fd);
    }
    return fd;
}

/*
 * 从 MOMIUI_USB_FD 取「同进程继承」的 fd 号。
 * 魔改 libfastboot.so 在 socket 路径失败后会走这个分支（0x298ba8）；
 * App 侧 UsbDeviceConnection.getFileDescriptor() 的 usbfs fd 通常不带 CLOEXEC，
 * ProcessBuilder fork+exec 后 fd 号不变，子进程可直接沿用。
 */
static int recv_inherited_fd(const char* fd_str) {
    char* end = NULL;
    errno = 0;
    long v = strtol(fd_str, &end, 10);
    if (errno != 0 || end == fd_str || *end != '\0' || v < 0 || v > INT_MAX) {
        diag("MOMIUI_USB_FD=\"%s\" 不是合法 fd 号", fd_str);
        return FDPASS_RECV_ERR_BADFD;
    }
    int fd = (int)v;
    if (fcntl(fd, F_GETFD) == -1) {
        diag("MOMIUI_USB_FD=%d 在本进程无效（跨 exec 未继承）errno=%d", fd, errno);
        return FDPASS_RECV_ERR_BADFD;
    }
    char link[64], target[PATH_MAX + 1], devfs[PATH_MAX + 1];
    snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    ssize_t rn = readlink(link, target, sizeof(target) - 1);
    if (rn > 0) {
        target[rn] = '\0';
        int is_usbfs = (strncmp(target, "/dev/bus/usb/", 13) == 0);
        /* 只诊断、不硬拒绝：fd 是否为 usbfs 由上层 read()+filter_usb_device 判定，
         * 这里硬失败会误伤 host 互通测试（用 pipe 模拟 fd），也可能误杀特殊 ROM。 */
        diag("退化使用 MOMIUI_USB_FD=%d → %s%s", fd, target,
             is_usbfs ? " [usbfs OK]" : " [非 usbfs! 上层描述符解析会失败]");
    } else {
        diag("退化使用 MOMIUI_USB_FD=%d（/proc/self/fd 不可反查）", fd);
    }
    fdpass_devfs_path(fd, devfs, sizeof(devfs));
    return fd;
}

int fdpass_recv_usbfs_fd(void) {
    const char* sock_name = getenv(FDPASS_ENV_SOCK);
    const char* fd_str = getenv(FDPASS_ENV_FD);
    int sock_rc = FDPASS_RECV_NO_ENV;

    /* 1) 首选：抽象 socket + SCM_RIGHTS（跨进程可靠通道） */
    if (sock_name != NULL && sock_name[0] != '\0') {
        int s = connect_abstract(sock_name);
        if (s >= 0) {
            int fd = receive_fd(s);
            close(s);
            if (fd >= 0) return fd;
            sock_rc = fd;  /* 记住 socket 通道失败原因，供回退时诊断 */
        } else {
            sock_rc = s;
        }
    }

    /* 2) 退化：MOMIUI_USB_FD 直传 fd 号。
     *    对齐魔改 libfastboot.so 的 0x298ba8 分支 —— 它并不要求 socket 成功，
     *    只要 MOMIUI_USB_FD 非空就 strtol 使用。诊断日志「socket 未就绪但有 FB=5」
     *    正是这一路径，原实现直接返回 socket 错误码把它丢掉了。 */
    if (fd_str != NULL && fd_str[0] != '\0') {
        diag("socket 通道不可用 (ret=%d)，尝试 MOMIUI_USB_FD 退化路径", sock_rc);
        return recv_inherited_fd(fd_str);
    }

    /* 3) 既没成功也没退化可用：返回 socket 错误码（未注入时是 NO_ENV） */
    return sock_rc;
}

int fdpass_devfs_path(int fd, char* out, size_t out_size) {
    char link[64];
    char buf[PATH_MAX + 1];
    snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    ssize_t n = readlink(link, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        /* 只接受真实 usbfs 路径，避免把普通文件误当设备 */
        if (strncmp(buf, "/dev/bus/usb/", 13) == 0) {
            snprintf(out, out_size, "%s", buf);
            return 0;
        }
    }
    snprintf(out, out_size, "/proc/self/fd/%d", fd);
    return -1;
}

static int read_int_file(const char* path, int* value) {
    FILE* f = fopen(path, "r");
    if (f == NULL) return -1;
    int ok = (fscanf(f, "%d", value) == 1);
    fclose(f);
    return ok ? 0 : -1;
}

int fdpass_sysfs_name(int fd, char* out, size_t out_size) {
    char devfs[PATH_MAX + 1];
    if (fdpass_devfs_path(fd, devfs, sizeof(devfs)) != 0) return -1;

    int bus = 0, dev = 0;
    if (sscanf(devfs, "/dev/bus/usb/%d/%d", &bus, &dev) != 2) return -1;

    DIR* dir = opendir("/sys/bus/usb/devices");
    if (dir == NULL) return -1;
    struct dirent* de;
    int found = -1;
    while ((de = readdir(dir)) != NULL) {
        char path[PATH_MAX];
        int b = 0, d = 0;
        snprintf(path, sizeof(path), "/sys/bus/usb/devices/%s/busnum", de->d_name);
        if (read_int_file(path, &b) != 0) continue;
        snprintf(path, sizeof(path), "/sys/bus/usb/devices/%s/devnum", de->d_name);
        if (read_int_file(path, &d) != 0) continue;
        if (b == bus && d == dev) {
            snprintf(out, out_size, "%s", de->d_name);
            found = 0;
            break;
        }
    }
    closedir(dir);
    return found;
}
