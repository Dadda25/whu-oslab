#include "riscv.h"
#include "dev/timer.h"

__attribute__ ((aligned (16))) uint8 CPU_stack[4096 * NCPU];

// 前向声明
void main();

void start()
{
    // 设置页表项为0，禁止分页
    w_satp(0);

    // 设置PMP地址寄存器0
    // 设置PMP配置寄存器0
    unsigned long x = r_mstatus();
    x &= ~MSTATUS_MPP_MASK;
    x |= MSTATUS_MPP_S;
    w_mstatus(x);

    // 把mepc设为main函数的地址
    w_mepc((uint64)main);

    // 关闭所有中断和异常
    w_medeleg(0xffff);
    w_mideleg(0xffff);
    w_sie(r_sie() | SIE_SEIE | SIE_STIE | SIE_SSIE);

    // 物理内存设置，从而设置到S模式
    // 能够访问所有物理内存
    w_pmpaddr0(0x3fffffffffffffull);
    w_pmpcfg0(0xf);

    // 请求时钟中断的启动
    timer_init();

    // 将每个CPU的hartid存放在寄存器中，从而供cpuid()使用
    int id = r_mhartid();
    w_tp(id);

    // 启用mret，跳转到main
    asm volatile("mret");
}