# host 侧验证：编译并运行 fdpass 接收端 ↔ 自有 fdpass 发送端 的 SCM_RIGHTS 互通测试。
# 完整 Android 静态 fastboot 请用 ./build.sh（需要 NDK）。
CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror -pthread
INCLUDES = -I fdpass
BUILD = build-host
TARGET = $(BUILD)/fdpass_interop_test

.PHONY: all check clean

all: $(TARGET)

$(TARGET): tests/fdpass_interop_test.c fdpass/fdpass_recv.c fdpass/fdpass_recv.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) tests/fdpass_interop_test.c fdpass/fdpass_recv.c -o $@

check: $(TARGET)
	./$(TARGET)

clean:
	rm -rf $(BUILD)
