#include "lib/print.h"
#include "trap/trap.h"
#include "mem/vmem.h"
#include "memlayout.h"
#include "proc/cpu.h"
#include "syscall/syscall.h"
#include "riscv.h"

// in trampoline.S
extern char trampoline[];      // 内核和用户切换的代码
extern char user_vector[];     // 用户触发trap进入内核
extern char user_return[];     // trap处理完毕返回用户

// in trap.S
extern char kernel_vector[];   // 内核态trap处理流程

// in trap_kernel.c
extern char* interrupt_info[16]; // 中断错误信息
extern char* exception_info[16]; // 异常错误信息

// 在user_vector()里面调用
// 用户态trap处理的核心逻辑
void trap_user_handler()
{
    uint64 sepc = r_sepc();          // 记录了发生异常时的pc值
    uint64 sstatus = r_sstatus();    // 与特权模式和中断相关的状态信息
    uint64 scause = r_scause();      // 引发trap的原因
    uint64 stval = r_stval();        // 发生trap时保存的附加信息(不同trap不一样)
    proc_t* p = myproc();

    // 确认trap来自U-mode
    assert((sstatus & SSTATUS_SPP) == 0, "trap_user_handler: not from u-mode");

    // send interrupts and exceptions to kerneltrap(),
    // since we're now in the kernel.
    w_stvec((uint64)kernel_vector);

    // save user program counter.
    p->tf->epc = sepc;

    // 针对scause制定的一系列规则
    int trap_id = scause & 0xf;
    bool isInterrupt = ((scause & ((uint64)1 << 63)) != 0);
    char* info = isInterrupt ? interrupt_info[trap_id] : exception_info[trap_id];

    if(scause == 8){
        // system call
        // if(killed(p)) exit(-1);

        // sepc points to the ecall instruction,
        // but we want to return to the next instruction.
        p->tf->epc += 4;

        // an interrupt will change sepc, scause, and sstatus,
        // so enable only now that we're done with those registers.
        intr_on();

        syscall();
    } else if (isInterrupt) {
        switch (trap_id)
        {
        case 1:
            // 处理时钟中断
            timer_interrupt_handler();
            break;
        case 5:
            // 处理计时器中断
            // Pass anyway...
            break;
        case 9:
            // 处理外部中断
            external_interrupt_handler();
            break;
        default:
            printf("usertrap(): unexpected interrupt at pid=%d\n", p->pid);
            printf("            trap id: %d trap info: %s\n", trap_id, info);
            printf("            scause %p\n", scause);
            printf("            sepc=%p stval=%p\n", sepc, stval);
            panic("usertrap: unexpected interrupt");
            break;
        }
    } else {
        // 异常处理
        printf("usertrap(): exception at pid=%d\n", p->pid);
        printf("            trap id: %d trap info: %s\n", trap_id, info);
        printf("            scause %p\n", scause);
        printf("            sepc=%p stval=%p\n", sepc, stval);
        panic("usertrap: unexpected exception");
        // setkilled(p);
    }

    trap_user_return();
}

// 调用user_return()
// 内核态返回用户态
void trap_user_return()
{
    struct proc *p = myproc();

    intr_off();

    // send syscalls, interrupts, and exceptions to uservec in trampoline.S
    uint64 trampoline_uservec = TRAMPOLINE + (user_vector - trampoline);
    w_stvec(trampoline_uservec);

    p->tf->kernel_satp = r_satp();         // kernel page table
    p->tf->kernel_sp = p->kstack + PGSIZE; // process's kernel stack
    p->tf->kernel_trap = (uint64)trap_user_handler;
    p->tf->kernel_hartid = r_tp();         // hartid for cpuid()

    // set S Previous Privilege mode to User.
    unsigned long x = r_sstatus();
    x &= ~SSTATUS_SPP; // clear SPP to 0 for user mode
    x |= SSTATUS_SPIE; // enable interrupts in user mode
    w_sstatus(x);

    // set S Exception Program Counter to the saved user pc.
    w_sepc(p->tf->epc);

    // tell trampoline.S the user page table to switch to.
    uint64 satp = MAKE_SATP(p->pgtbl);

    uint64 trampoline_userret = TRAMPOLINE + (user_return - trampoline);
    ((void (*)(uint64, uint64))trampoline_userret)(TRAPFRAME, satp);
}