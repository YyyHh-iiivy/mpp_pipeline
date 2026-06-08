/*
 * Copyright (c) 2006-2021, RT-Thread Development Team
 *
 * Change Logs:
 * Date           Author         Notes
 * 2024-12-05     weidongshan    first version
 */
 
#include <rtthread.h>
#include <rtdevice.h>
#include <stdio.h>
#include <string.h>

#define THREAD_PRIORITY         15  //设置线程优先级
#define THREAD_STACK_SIZE       2048 //设置线程栈大小
#define THREAD_TIMESLICE        15  //设置线程时间片大小

/* =======================================================
 * 线程1：静态创建所需的数据结构
 * ======================================================= */
/* 1. 静态分配线程控制块 (TCB) */
static struct rt_thread thread1_tcb;

/* 2. 静态分配线程栈。必须使用 ALIGN 宏保证内存地址按照 RT_ALIGN_SIZE 对齐 */
ALIGN(RT_ALIGN_SIZE)
static rt_uint8_t thread1_stack[THREAD_STACK_SIZE];


/* =======================================================
 * 线程2：动态创建所需的数据结构
 * ======================================================= */
/* 动态线程仅需分配一个指向 TCB 的指针句柄 */
static rt_thread_t thread2;


/* 线程1的入口函数 */
static void thread1_entry(void *parameter)
{
    const char *thread_name = "Thread1 run (Static)\r\n";
    
    while(1)
    {
        rt_kprintf(thread_name);
        rt_thread_mdelay(100);
    }
}

/* 线程2入口函数 */
static void thread2_entry(void *param)
{
    const char *thread_name = "Thread2 run (Dynamic)\r\n";
    
    while(1)
    {
        rt_kprintf(thread_name);
        rt_thread_mdelay(100);
    }
}

int main(void)
{
    rt_err_t result;

    /* 初始化静态线程1，将 TCB、栈内存与入口函数进行系统级绑定 */
    result = rt_thread_init(&thread1_tcb,
                            "thread1",
                            thread1_entry,
                            RT_NULL,
                            &thread1_stack[0],      // 静态分配的栈起始地址
                            sizeof(thread1_stack),  // 栈大小
                            THREAD_PRIORITY,
                            THREAD_TIMESLICE);

    /* 启动线程1 */
    if (result == RT_EOK)
    {
        rt_thread_startup(&thread1_tcb);       
    }
    else
    {
        rt_kprintf("线程1静态初始化失败\n");
        return -1;
    }
    
    /* 创建动态线程2，系统将从内核堆中通过 malloc 机制分配 TCB 和 栈空间 */
    thread2 = rt_thread_create("thread2",          
                               thread2_entry,      
                               RT_NULL,            
                               THREAD_STACK_SIZE,  
                               THREAD_PRIORITY,    
                               THREAD_TIMESLICE);  

    if (thread2 != RT_NULL)
    {
        rt_thread_startup(thread2);
    }
    else
    {
        rt_kprintf("线程2动态创建失败\n");
        return -1;
    }

    /* 主线程挂起循环 */
    while (1)
    {
        rt_thread_mdelay(100);
    }   
        
    return 0;
}