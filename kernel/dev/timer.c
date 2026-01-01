#include "lib/lock.h"
#include "lib/print.h"
#include "dev/timer.h"
#include "memlayout.h"
#include "riscv.h"

/*-------------------- 工作在M-mode --------------------*/

// in trap.S M-mode时钟中断处理流程()
extern void timer_vector();

// 每个CPU在时钟中断中需要的临时空间(考虑为什么可以这么写)
static uint64 mscratch[NCPU][5];

// 时钟初始化
// called in start.c
void timer_init()
{
    // 每个CPU核都有各自的时钟中断
    int id = r_mhartid();

    // 请求CLINT时钟中断
    *(uint64*)CLINT_MTIMECMP(id) = *(uint64*)CLINT_MTIME + INTERVAL;

    // 为timervec准备对应的信息
    // scratch[0...2]：用于临时存放寄存器值的空间
    // scratch[3]：存放CLINT_MTIMECMP
    // scratch[4]：时钟中断的区间
    uint64 *scratch = &mscratch[id][0];
    scratch[3] = CLINT_MTIMECMP(id);
    scratch[4] = INTERVAL;
    w_mscratch((uint64)scratch);

    // 启动M模式中断处理
    w_mtvec((uint64)timer_vector);

    // 启动M模式中断
    w_mstatus(r_mstatus() | MSTATUS_MIE);

    // 启动M模式对应的时钟中断
    w_mie(r_mie() | MIE_MTIE);
}


/*--------------------- 工作在S-mode --------------------*/

// 系统时钟
static timer_t sys_timer;

// 时钟创建(初始化系统时钟)
void timer_create()
{
    spinlock_init(&sys_timer.lk, "time");
    sys_timer.ticks = 0;
}

// 时钟更新(ticks++ with lock)
void timer_update()
{
    spinlock_acquire(&sys_timer.lk);
    sys_timer.ticks++;
    spinlock_release(&sys_timer.lk);
}

// 返回系统时钟ticks
uint64 timer_get_ticks()
{
    return sys_timer.ticks;
}