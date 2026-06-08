# 1. 定义交叉编译器的绝对路径
TOOLCHAIN_DIR = /home/ubuntu/k230_sdk/toolchain/riscv64-linux-musleabi_for_x86_64-pc-linux-gnu/bin
CC = $(TOOLCHAIN_DIR)/riscv64-unknown-linux-musl-gcc

# 编译选项：开启所有警告并进行 O2 级别优化
CFLAGS = -Wall -O2

# 目标文件与源文件
TARGET = hello.elf
SRCS = hello.c

# 默认编译规则
all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $(SRCS) -o $(TARGET)
	@echo "编译完成！大核可执行文件: $(TARGET)"

# 清理规则
clean:
	rm -f $(TARGET)
	@echo "清理完毕。"