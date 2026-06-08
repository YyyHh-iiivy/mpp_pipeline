#include <rtthread.h>
#include <stdio.h>

#define THREAD_PRIORITY         25      
#define THREAD_STACK_SIZE       8192    
#define THREAD_TIMESLICE        5       

// 声明一个全局标志位，初始为 0（未完成）
volatile int g_thread_is_done = 0;

static void my_thread_entry(void *parameter) {
    int count = 0;
    while (count < 10) {
        printf("[My Thread] 正在运行! 当前计数 = %d\n", count);
        count++;
        rt_thread_mdelay(500); 
    }
    printf("[My Thread] 工作完成，自动退出并释放内存。\n");
    
    // 子线程干完活了，修改标志位通知主线程
    g_thread_is_done = 1;
}

int run_thread_test(void) {
    rt_thread_t tid = RT_NULL; 
    
    // 每次运行前重置标志位
    g_thread_is_done = 0;

    printf("--- 开始创建动态线程 ---\n");

    tid = rt_thread_create("test_th", my_thread_entry, RT_NULL, 
                           THREAD_STACK_SIZE, THREAD_PRIORITY, THREAD_TIMESLICE);

    if (tid != RT_NULL) {
        rt_thread_startup(tid); 
        printf("动态线程创建并启动成功！主线程进入等待...\n");
    } else {
        printf("严重错误：动态线程创建失败 (可能是内存不足)！\n");
        return -1;
    }

    return 0;
}