/*
 * fdpass_interop_test.c — host 侧真实 SCM_RIGHTS 互通测试。
 *
 * 目的：证明「自有 fdpass 发送端」与「本仓库新增的接收端」协议完全互通，
 *      即 fastboot 二进制能收下 App 侧 libfdpass.so 传来的 usbfs fd。
 *
 * 内置一个最小发送端，逐条复刻 xioo0317/fdpass::fdpass_core.c 的 serveFdLoop：
 *   抽象 socket 地址(sun_path[0]=0, '@' 后名字复制到 sun_path+1, addrlen=len+3)
 *   → listen(128) → accept → poll(2s) → read(1B) → dup(fd)
 *   → sendmsg: 1 字节数据 0x66 + SCM_RIGHTS(cmsg_len=20) → 关闭副本
 *
 * 验证：
 *   1) 环境变量未设置 → fdpass_recv_usbfs_fd() 立即返回 FDPASS_RECV_NO_ENV
 *   2) 正常传递 → 收到的 fd 与发送端 pipe 是同一内核对象（写入 'Z' 能读回）
 *   3) 名字必须以 '@' 开头 → 非法名返回 FDPASS_RECV_ERR_NAME
 *   4) MOMIUI_USB_FD 退化路径可用
 *   5) CMSG_LEN/CMSG_SPACE 与 arm64 契约一致 (20 / 24)
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "fdpass_recv.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define TEST_SOCK_NAME "@fba_interop_test"
#define DATA_BYTE 0x66

typedef struct {
    const char* name;
    int fd_to_send;
    volatile int listening;
    volatile int served;
    int err;
} server_ctx;

static void* mini_server(void* arg) {
    server_ctx* c = (server_ctx*)arg;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    addr.sun_path[0] = '\0';
    const char* tail = c->name + 1;
    size_t n = strlen(tail);
    memcpy(addr.sun_path + 1, tail, n);
    socklen_t addrlen = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + n);

    int lfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (lfd < 0) { c->err = 1; return NULL; }
    if (bind(lfd, (const struct sockaddr*)&addr, addrlen) < 0) {
        c->err = 2; close(lfd); return NULL;
    }
    if (listen(lfd, 128) < 0) { c->err = 3; close(lfd); return NULL; }
    c->listening = 1;

    int client = accept(lfd, NULL, NULL);
    if (client < 0) { c->err = 4; close(lfd); return NULL; }

    char req;
    ssize_t ignored = read(client, &req, 1);
    (void)ignored;

    int copy = dup(c->fd_to_send);
    char byte = (char)DATA_BYTE;
    struct iovec iov = { .iov_base = &byte, .iov_len = 1 };
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
    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &copy, sizeof(int));

    if (sendmsg(client, &msg, 0) != 1) c->err = 5;
    close(copy);
    close(client);
    close(lfd);
    c->served = 1;
    return NULL;
}

static void wait_listening(server_ctx* c) {
    for (int i = 0; i < 200 && !c->listening; ++i) usleep(10 * 1000);
}

static int test_no_env(void) {
    unsetenv(FDPASS_ENV_SOCK);
    unsetenv(FDPASS_ENV_FD);
    int r = fdpass_recv_usbfs_fd();
    printf("[no-env]      ret=%d (期望 %d)\n", r, FDPASS_RECV_NO_ENV);
    return r == FDPASS_RECV_NO_ENV ? 0 : 1;
}

static int test_bad_name(void) {
    setenv(FDPASS_ENV_SOCK, "not_abstract", 1);
    unsetenv(FDPASS_ENV_FD);
    int r = fdpass_recv_usbfs_fd();
    printf("[bad-name]    ret=%d (期望 %d)\n", r, FDPASS_RECV_ERR_NAME);
    unsetenv(FDPASS_ENV_SOCK);
    return r == FDPASS_RECV_ERR_NAME ? 0 : 1;
}

static int test_real_transfer(void) {
    int pipefd[2];
    if (pipe(pipefd) != 0) return 1;

    server_ctx c = { TEST_SOCK_NAME, pipefd[0], 0, 0, 0 };
    pthread_t th;
    pthread_create(&th, NULL, mini_server, &c);
    wait_listening(&c);

    setenv(FDPASS_ENV_SOCK, TEST_SOCK_NAME, 1);
    unsetenv(FDPASS_ENV_FD);
    int got = fdpass_recv_usbfs_fd();
    pthread_join(th, NULL);
    unsetenv(FDPASS_ENV_SOCK);

    printf("[transfer]    got_fd=%d served=%d server_err=%d\n", got, c.served, c.err);
    if (got < 0 || c.err != 0 || !c.served) {
        close(pipefd[0]); close(pipefd[1]); return 1;
    }

    /* 往发送端 pipe 写 'Z'，从收到的 fd 读回 → 证明是同一个内核对象 */
    int ok = 0;
    if (write(pipefd[1], "Z", 1) == 1) {
        char out = 0;
        if (read(got, &out, 1) == 1 && out == 'Z') ok = 1;
    }
    printf("[same-object] %s\n", ok ? "OK" : "FAIL");
    close(got);
    close(pipefd[0]);
    close(pipefd[1]);
    return ok ? 0 : 1;
}

static int test_env_fd_fallback(void) {
    int pipefd[2];
    if (pipe(pipefd) != 0) return 1;
    char num[32];
    snprintf(num, sizeof(num), "%d", pipefd[0]);
    unsetenv(FDPASS_ENV_SOCK);
    setenv(FDPASS_ENV_FD, num, 1);
    int got = fdpass_recv_usbfs_fd();
    printf("[env-fd]      got_fd=%d (期望 %d)\n", got, pipefd[0]);
    unsetenv(FDPASS_ENV_FD);
    int ok = (got == pipefd[0]);
    close(pipefd[0]); close(pipefd[1]);
    return ok ? 0 : 1;
}

/*
 * 真机诊断日志「socket 未在 1.5s 内就绪，但有 MOMIUI_USB_FD=5」的场景：
 * socket 名合法却无人 listen，工具侧必须回退到 MOMIUI_USB_FD，
 * 而不是直接返回 connect 错误码让 find_usb_device 掉进 < waiting for any device >。
 * 用最小重试次数把 101×20ms 缩短，避免测试拖慢 2 秒。
 */
static int test_socket_fail_fallback_fd(void) {
    int pipefd[2];
    if (pipe(pipefd) != 0) return 1;
    char num[32];
    snprintf(num, sizeof(num), "%d", pipefd[0]);
    setenv(FDPASS_ENV_SOCK, "@fba_no_such_server_for_test", 1);
    setenv(FDPASS_ENV_FD, num, 1);
    setenv("FDPASS_CONNECT_RETRIES", "2", 1);
    int got = fdpass_recv_usbfs_fd();
    printf("[sock-fail→fd] got_fd=%d (期望 %d)\n", got, pipefd[0]);
    unsetenv("FDPASS_CONNECT_RETRIES");
    unsetenv(FDPASS_ENV_SOCK);
    unsetenv(FDPASS_ENV_FD);
    int ok = (got == pipefd[0]);
    close(pipefd[0]); close(pipefd[1]);
    return ok ? 0 : 1;
}

/* 只有 socket 名、FD 无效时，应返回 socket 错误码而不是 NO_ENV */
static int test_socket_fail_no_fd(void) {
    setenv(FDPASS_ENV_SOCK, "@fba_no_such_server_for_test", 1);
    unsetenv(FDPASS_ENV_FD);
    setenv("FDPASS_CONNECT_RETRIES", "2", 1);
    int got = fdpass_recv_usbfs_fd();
    printf("[sock-fail]   ret=%d (期望 <0 且 != NO_ENV)\n", got);
    unsetenv("FDPASS_CONNECT_RETRIES");
    unsetenv(FDPASS_ENV_SOCK);
    return (got < 0 && got != FDPASS_RECV_NO_ENV) ? 0 : 1;
}

int main(void) {
    printf("fdpass 接收端 ↔ 自有 fdpass 发送端 互通测试\n");
    printf("CMSG_LEN=%zu CMSG_SPACE=%zu (arm64 契约 20/24)\n",
           (size_t)CMSG_LEN(sizeof(int)), (size_t)CMSG_SPACE(sizeof(int)));

    int fail = 0;
    fail += test_no_env();
    fail += test_bad_name();
    fail += test_real_transfer();
    fail += test_env_fd_fallback();
    fail += test_socket_fail_fallback_fd();
    fail += test_socket_fail_no_fd();

    if ((size_t)CMSG_LEN(sizeof(int)) != 20 || (size_t)CMSG_SPACE(sizeof(int)) != 24) {
        printf("[cmsg]        FAIL 尺寸不符\n");
        fail++;
    }
    printf("\n%s\n", fail == 0 ? "ALL PASS" : "SOME TESTS FAILED");
    return fail == 0 ? 0 : 1;
}
