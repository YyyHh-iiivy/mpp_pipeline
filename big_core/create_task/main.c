#include <stdio.h>
#include <string.h>
#include <rtthread.h> 

extern int run_thread_test(void);
extern volatile int g_thread_is_done; // 引入全局标志位

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("用法: ./big_app.elf [测试名称]\n");
        return -1;
    }

    if (strcmp(argv[1], "thread") == 0) { 
        if (run_thread_test() == 0) {
            // 轮询检查标志位，如果子线程没干完活，主线程就保持休眠
            while (g_thread_is_done == 0) {
                rt_thread_mdelay(100); 
            }
            printf("--- 收到子线程完工通知，主进程安全退出 ---\n");
        }
        return 0;
    }
    // ... 省略 else 分支
}