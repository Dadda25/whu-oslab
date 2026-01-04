#include "proc/cpu.h"
#include "riscv.h"
#include "lib/print.h"
#include "lib/str.h"

static cpu_t cpus[NCPU];

// 获取当前CPU结构体
cpu_t* mycpu(void)
{
    int id = r_tp();  // 读取tp寄存器获取hartid
    return &cpus[id];
}

// 获取当前CPU的ID
int mycpuid(void) 
{
    int id = r_tp();
    return id;
}

// 获取当前CPU上运行的进程
proc_t* myproc(void)
{
    push_off();  // 关中断，防止在读取过程中发生进程切换
    cpu_t* c = mycpu();
    proc_t* p = c->proc;
    pop_off();   // 开中断
    return p;
}