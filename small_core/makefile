CC = /home/ubuntu/k230_sdk/toolchain/Xuantie-900-gcc-linux-5.10.4-glibc-x86_64-V2.6.0/bin/riscv64-unknown-linux-gnu-gcc
CFLAGS = -Wall -O2

#SRCS = $(wildcard *.c) # 自动抓取所有 .c 文件 

# --- 白名单设置 (统一打包模式) ---
# 只把列出的 .c 文件合并编译成一个最终的可执行文件
SRCS = hello.c
       # broken_module.c  <-- 不想编译它，直接前面加个 # 注释掉

TARGET = small_app.elf

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $(SRCS) -o $(TARGET)
	@echo "小核编译打包成功！生成可执行文件: $(TARGET)"

clean:
	rm -f $(TARGET)