#include "lib/lock.h"
#include "proc/cpu.h"
#include "proc/proc.h"

// 初始化睡眠锁
void sleeplock_init(sleeplock_t* slk, char *name)
{
    spinlock_init(&slk->lk, "sleep lock");
    slk->name = name;
    slk->locked = 0;
    slk->pid = -1; // 由于proc_zero的进程号为0，所以这里设为-1
}

// 获取睡眠锁
void sleeplock_acquire(sleeplock_t* slk)
{
    spinlock_acquire(&slk->lk);
    while (slk->locked) {
        proc_sleep(slk, &slk->lk);
    }
    slk->locked = 1;
    slk->pid = myproc()->pid;
    spinlock_release(&slk->lk);
}

// 释放睡眠锁
void sleeplock_release(sleeplock_t* slk)
{
    spinlock_acquire(&slk->lk);
    slk->locked = 0;
    slk->pid = -1;
    proc_wakeup(slk);
    spinlock_release(&slk->lk);
}

// 检查睡眠锁是否被占用着
bool sleeplock_holding(sleeplock_t* slk)
{
    bool r;

    spinlock_acquire(&slk->lk);
    r = slk->locked && (slk->pid == myproc()->pid);
    spinlock_release(&slk->lk);
    return r;
}