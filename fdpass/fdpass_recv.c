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

/* connect 抽象 socket；服务端可能晚于本进程启动，需重试 */
static int connect_abstract(const char* name) {
    struct sockaddr_un addr;
    socklen_t addrlen;
    int rc = make_addr(name, &addr, &addrlen);
    if (rc != 0) return rc;

    for (int attempt = 0; attempt < CONNECT_RETRIES; ++attempt) {
        int s = socket(AF_UNIX, SOCK_STREAM, 0);
        if (s < 0) return FDPASS_RECV_ERR_SOCKET;
        if (connect(s, (const struct sockaddr*)&addr, addrlen) == 0) return s;
        int e = errno;
        close(s);
        /* 未 listen / 尚未 bind 都表现为 ECONNREFUSED 或 ENOENT，继续重试 */
        if (e != ECONNREFUSED && e != ENOENT && e != EAGAIN) return FDPASS_RECV_ERR_CONNECT;
        nap_ms(CONNECT_RETRY_MS);
    }
    return FDPASS_RECV_ERR_CONNECT;
}

/* write 1 字节请求 → recvmsg 取 SCM_RIGHTS fd */
static int receive_fd(int sock) {
    char req = (char)REQ_BYTE;
    if (write(sock, &req, 1) != 1) return FDPASS_RECV_ERR_WRITE;

    struct pollfd pfd = { .fd = sock, .events = POLLIN, .revents = 0 };
    int pr = poll(&pfd, 1, RECV_TIMEOUT_MS);
    if (pr <= 0) return FDPASS_RECV_ERR_RECVMSG;

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
    if (n < 0) return FDPASS_RECV_ERR_RECVMSG;

    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg == NULL || cmsg->cmsg_level != SOL_SOCKET ||
        cmsg->cmsg_type != SCM_RIGHTS || cmsg->cmsg_len < CMSG_LEN(sizeof(int))) {
        return FDPASS_RECV_ERR_NOFD;
    }
    int fd;
    memcpy(&fd, CMSG_DATA(cmsg), sizeof(fd));
    if (fd < 0 || fcntl(fd, F_GETFD) == -1) return FDPASS_RECV_ERR_BADFD;
    return fd;
}

int fdpass_recv_usbfs_fd(void) {
    const char* sock_name = getenv(FDPASS_ENV_SOCK);
    if (sock_name != NULL && sock_name[0] != '\0') {
        int s = connect_abstract(sock_name);
        if (s < 0) return s;
        int fd = receive_fd(s);
        close(s);
        return fd;
    }

    /* 退化路径：同进程直传 fd 号（跨进程因 fd 重编号通常无效，保留兼容） */
    const char* fd_str = getenv(FDPASS_ENV_FD);
    if (fd_str != NULL && fd_str[0] != '\0') {
        char* end = NULL;
        errno = 0;
        long v = strtol(fd_str, &end, 10);
        if (errno != 0 || end == fd_str || *end != '\0' || v < 0 || v > INT_MAX)
            return FDPASS_RECV_ERR_BADFD;
        int fd = (int)v;
        if (fcntl(fd, F_GETFD) == -1) return FDPASS_RECV_ERR_BADFD;
        return fd;
    }

    return FDPASS_RECV_NO_ENV;
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
