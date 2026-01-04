#include "lib/lock.h"
#include "lib/print.h"
#include "proc/cpu.h"
#include "riscv.h"

// 带层数叠加的关中断
void push_off(void)
{
    int old = intr_get();

    intr_off();
    if(mycpu()->noff == 0)
        mycpu()->origin = old;
    mycpu()->noff += 1;
}

// 带层数叠加的开中断
void pop_off(void)
{
    struct cpu *c = mycpu();
    if(intr_get())
        panic("pop_off - interruptible");
    if(c->noff < 1)
        panic("pop_off");
    c->noff -= 1;
    if(c->noff == 0 && c->origin)
        intr_on();
}

// 是否持有自旋锁
// 中断应当是关闭的
bool spinlock_holding(spinlock_t *lk)
{
    int r; 
    r = (lk->locked && lk->cpuid == mycpuid());
    return r;
}

// 自选锁初始化
void spinlock_init(spinlock_t *lk, char *name)
{
    lk->name = name;
    lk->locked = 0;
    lk->cpuid = -1;
}

// 获取自旋锁
void spinlock_acquire(spinlock_t *lk)
{    
    push_off(); // disable interrupts to avoid deadlock.
    if(spinlock_holding(lk))
        panic("acquire");
    
    // On RISC-V, sync_lock_test_and_set turns into an atomic swap:
    //   a5 = 1
    //   s1 = &lk->locked
    //   amoswap.w.aq a5, a5, (s1)
    while(__sync_lock_test_and_set(&lk->locked, 1) != 0)
    ;

    // Tell the C compiler and the processor to not move loads or stores
    // past this point, to ensure that the critical section's memory
    // references happen strictly after the lock is acquired.
    // On RISC-V, this emits a fence instruction.
    __sync_synchronize();

    // Record info about lock acquisition for spinlock_holding() and debugging.
    lk->cpuid = mycpuid();
} 

// 释放自旋锁
void spinlock_release(spinlock_t *lk)
{
    if(!spinlock_holding(lk))
        panic("release");

    lk->cpuid = -1;

    // Tell the C compiler and the CPU to not move loads or stores
    // past this point, to ensure that all the stores in the critical
    // section are visible to other CPUs before the lock is released,
    // and that loads in the critical section occur strictly before
    // the lock is released.
    // On RISC-V, this emits a fence instruction.
    __sync_synchronize();

    // Release the lock, equivalent to lk->locked = 0.
    // This code doesn't use a C assignment, since the C standard
    // implies that an assignment might be implemented with
    // multiple store instructions.
    // On RISC-V, sync_lock_release turns into an atomic swap:
    //   s1 = &lk->locked
    //   amoswap.w zero, zero, (s1)
    __sync_lock_release(&lk->locked);

    pop_off();
}