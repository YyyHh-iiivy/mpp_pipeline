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


static struct rt_thread *thread1;    //定义线程1句柄
static struct rt_thread *thread2;   //定义线程2句柄指针

/* 线程1的入口函数 */

static void thread1_entry(void *parameter)
{
    const char *thread_name = "Thread1 run\r\n";
    /* 线程1 */
    while(1)
    {
        /* 打印线程1的信息 */
        rt_kprintf(thread_name);

        /* 延迟一会，让出CPU */
        rt_thread_mdelay(100);
    }
}

/* 线程2入口函数 */
static void thread2_entry(void *param)
{
    const char *thread_name = "Thread2 run\r\n";
    /* 线程2 */
    while(1)
    {
        /* 打印线程2的信息 */
        rt_kprintf(thread_name);

        /* 延迟一会，让出CPU */
        rt_thread_mdelay(100);
    }
}

int main(void)
{
/* 初始化动态线程1，名称是Thread1，入口是thread1_entry */
     thread1 = rt_thread_create("thread1",          // 线程名字
                                thread1_entry,     // 入口函数
                                RT_NULL,           // 入口函数参数
                                THREAD_STACK_SIZE, // 栈大小
                                THREAD_PRIORITY,   // 线程优先级
                                THREAD_TIMESLICE); // 线程时间片大小

/* 启动线程1 */
    if (thread1 != RT_NULL)
    rt_thread_startup(thread1);       

/* 创建动态线程2，名称是thread2，入口是thread2_entry*/
    thread2 = rt_thread_create("thread2",          //线程名字
                            thread2_entry,     //入口函数
                            RT_NULL,           //入口函数参数
                            THREAD_STACK_SIZE, //栈大小
                            THREAD_PRIORITY,   //线程优先级
                            THREAD_TIMESLICE); //线程时间片大小

    if (thread2 != RT_NULL)
    rt_thread_startup(thread2);
    
    rt_err_t result1 =  rt_thread_delete(thread1); 
    if(result1 == RT_EOK)
	rt_kprintf("Thread1 exit\r\n");

    rt_err_t result2 =  rt_thread_delete(thread2); 
    if(result2 == RT_EOK)
	rt_kprintf("Thread2 exit\r\n");
    
    /* 主线程挂起循环 */
    while (1)
    {
        rt_thread_mdelay(100);
    }   
        
    return 0;
}