#include "lib/print.h"
#include "lib/str.h"
#include "mem/pmem.h"
#include "mem/vmem.h"
#include "mem/mmap.h"
#include "proc/cpu.h"
#include "proc/initcode.h"
#include "memlayout.h"
#include "riscv.h"
#include "common.h"

/*----------------外部空间------------------*/

// in trampoline.S
extern char trampoline[];

// in swtch.S
extern void swtch(context_t* old, context_t* new);

// in trap_user.c
extern void trap_user_return();

/*----------------本地变量------------------*/

// 进程数组
proc_t proc[NPROC];  // 改成全局数组，与模板一致

// 第一个进程的指针
static proc_t* proczero;

// 全局的pid和保护它的锁 
static int nextpid = 1;
static spinlock_t pid_lock;

// wait的自旋锁
static spinlock_t wait_lock;

// 申请一个pid(锁保护)
static int allocpid()
{
    int pid;
    spinlock_acquire(&pid_lock);
    pid = nextpid;
    nextpid = nextpid + 1;
    spinlock_release(&pid_lock);
    return pid;
}

// 释放锁 + 调用 trap_user_return
static void fork_return()
{
    // 由于调度器中上了锁，所以这里需要解锁
    proc_t* p = myproc();
    spinlock_release(&p->lk);
    trap_user_return();
}

// 返回一个未使用的进程空间
proc_t* proc_alloc()
{
    proc_t* p;
    
    // 查找一个UNUSED状态的进程槽
    for(p = proc; p < &proc[NPROC]; p++) {
        spinlock_acquire(&p->lk);
        if(p->state == UNUSED) {
            goto found;  // 找到后跳出，持有锁
        } else {
            spinlock_release(&p->lk);
        }
    }
    return 0;  // 没有空闲进程槽
    
found:
    // 分配pid
    p->pid = allocpid();
    p->state = RUNNABLE;
    
    // 分配trapframe物理页
    p->tf = (trapframe_t*)pmem_alloc(PMEM_USER);
    if(p->tf == 0) {
        proc_free(p);
        spinlock_release(&p->lk);
        return 0;
    }
    
    // 分配页表
    p->pgtbl = proc_pgtbl_init((uint64)p->tf);
    if(p->pgtbl == 0) {
        proc_free(p);
        spinlock_release(&p->lk);
        return 0;
    }
    
    // 设置内核栈地址
    p->kstack = KSTACK((int)(p - proc));
    
    // 设置context
    memset(&p->ctx, 0, sizeof(context_t));
    p->ctx.ra = (uint64)fork_return;
    p->ctx.sp = p->kstack + PGSIZE;
    
    return p;  // 返回时持有锁
}

// 释放一个进程空间
void proc_free(proc_t* p)
{
    if(p->tf)
        pmem_free((uint64)p->tf, PMEM_USER);
    p->tf = 0;
    
    if(p->pgtbl)
        uvm_destroy_pgtbl(p->pgtbl);
    p->pgtbl = 0;
    
    // 释放mmap区域链表
    mmap_region_t* curr = p->mmap;
    while(curr != NULL) {
        mmap_region_t* next = curr->next;
        mmap_region_free(curr);
        curr = next;
    }
    p->mmap = NULL;
    
    // 重置其他字段
    p->pid = 0;
    p->state = UNUSED;
    p->parent = NULL;
    p->exit_state = 0;
    p->sleep_space = NULL;
    p->heap_top = 0;
    p->ustack_pages = 0;
    p->kstack = 0;
    memset(&p->ctx, 0, sizeof(context_t));
}

// 进程模块初始化
void proc_init()
{
    proc_t* p;
    
    spinlock_init(&pid_lock, "nextpid");
    spinlock_init(&wait_lock, "wait_lock");
    
    for(p = proc; p < &proc[NPROC]; p++) {
        spinlock_init(&p->lk, "proc");
        p->state = UNUSED;
        p->kstack = KSTACK((int)(p - proc));
    }
    
    printf("proc_init: process system initialized\n");
}

// 获得一个初始化过的用户页表
pgtbl_t proc_pgtbl_init(uint64 trapframe)
{
    pgtbl_t pgtbl = (pgtbl_t)pmem_alloc(PMEM_KERNEL);
    if(pgtbl == 0)
        return 0;
    memset(pgtbl, 0, PGSIZE);
    
    // 映射trampoline页
    vm_mappages(pgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);
    
    // 映射trapframe页
    vm_mappages(pgtbl, TRAPFRAME, trapframe, PGSIZE, PTE_R | PTE_W);
    
    return pgtbl;
}

// 第一个用户态进程的创建
void proc_make_first()
{
    proc_t* p;
    
    p = proc_alloc();
    if(p == NULL) {
        panic("proc_make_first: proc_alloc failed");
    }
    proczero = p;
    
    printf("proc_make_first: initcode_len = %d bytes\n", initcode_len);
    
    // 映射代码段到虚拟地址 PGSIZE（跳过第一页）
    assert(initcode_len <= PGSIZE, "proc_make_first: initcode too big");
    char* mem = (char*)pmem_alloc(PMEM_USER);
    if(mem == 0) {
        panic("proc_make_first: code alloc failed");
    }
    memset(mem, 0, PGSIZE);
    
    // 代码段从 PGSIZE 开始
    vm_mappages(p->pgtbl, PGSIZE, (uint64)mem, PGSIZE, 
                PTE_W | PTE_R | PTE_X | PTE_U);
    memmove(mem, initcode, initcode_len);
    
    // 映射用户栈（在高地址）
    uint64 ustack_phys = (uint64)pmem_alloc(PMEM_USER);
    if(ustack_phys == 0) {
        panic("proc_make_first: stack alloc failed");
    }
    
    // 用户栈的虚拟地址（在用户地址空间的高处）
    uint64 stack_va = PGSIZE * 10;  // 不要用 kstack 来计算！
    vm_mappages(p->pgtbl, stack_va, ustack_phys, PGSIZE, 
                PTE_R | PTE_W | PTE_U);
    
    p->ustack_pages = 1;
    p->heap_top = PGSIZE * 2;  // 堆从代码段后开始
    p->parent = NULL;
    p->mmap = NULL;
    
    // 设置trapframe
    memset(p->tf, 0, sizeof(trapframe_t));
    p->tf->epc = PGSIZE;  // 从 PGSIZE 开始执行
    p->tf->sp = stack_va + PGSIZE;  // 栈顶
    
    printf("proc_make_first: code at 0x%x, stack at 0x%lx, sp=0x%lx\n",
           PGSIZE, stack_va, p->tf->sp);
    
    p->state = RUNNABLE;
    spinlock_release(&p->lk);
    
    printf("proc_make_first: first process created (pid=%d)\n", p->pid);
}

// 进程复制
int proc_fork()
{
    proc_t* parent = myproc();
    proc_t* child;
    int pid;
    
    printf("[fork] parent pid=%d, heap_top=0x%lx, ustack_pages=%d\n",
           parent->pid, parent->heap_top, parent->ustack_pages);
    
    child = proc_alloc();
    if(child == NULL) {
        return -1;
    }
    
    printf("[fork] child pid=%d allocated\n", child->pid);
    
    // 拷贝用户页表 (注意：参数顺序是 old, new)
    uvm_copy_pgtbl(parent->pgtbl, child->pgtbl, parent->heap_top, 
                   parent->ustack_pages, parent->mmap);
    
    printf("[fork] child pid=%d page table copied\n", child->pid);
    
    child->heap_top = parent->heap_top;
    child->ustack_pages = parent->ustack_pages;
    
    // 深拷贝 mmap
    mmap_region_t* parent_mmap = parent->mmap;
    mmap_region_t** child_mmap_ptr = &child->mmap;
    
    while(parent_mmap != NULL) {
        mmap_region_t* new_region = mmap_region_alloc();
        new_region->begin = parent_mmap->begin;
        new_region->npages = parent_mmap->npages;
        new_region->next = NULL;
        
        *child_mmap_ptr = new_region;
        child_mmap_ptr = &new_region->next;
        
        parent_mmap = parent_mmap->next;
    }
    
    // 拷贝trapframe
    *(child->tf) = *(parent->tf);
    child->tf->a0 = 0;  // 子进程返回0
    
    printf("[fork] child pid=%d: epc=0x%lx, sp=0x%lx\n",
           child->pid, child->tf->epc, child->tf->sp);
    
    pid = child->pid;
    
    spinlock_release(&child->lk);
    
    spinlock_acquire(&wait_lock);
    child->parent = parent;
    spinlock_release(&wait_lock);
    
    spinlock_acquire(&child->lk);
    child->state = RUNNABLE;
    spinlock_release(&child->lk);
    
    return pid;
}

// 进程放弃CPU的控制权
void proc_yield()
{
    proc_t* p = myproc();
    spinlock_acquire(&p->lk);
    p->state = RUNNABLE;
    proc_sched();
    spinlock_release(&p->lk);
}

// 等待子进程
int proc_wait(uint64 addr)
{
    proc_t* pp;
    int havekids, pid;
    proc_t* p = myproc();
    
    spinlock_acquire(&wait_lock);
    
    for(;;) {
        havekids = 0;
        for(pp = proc; pp < &proc[NPROC]; pp++) {
            if(pp->parent == p) {
                spinlock_acquire(&pp->lk);
                
                havekids = 1;
                if(pp->state == ZOMBIE) {
                    pid = pp->pid;
                    if(addr != 0) {
                        uvm_copyout(p->pgtbl, addr, (uint64)&pp->exit_state, sizeof(pp->exit_state));
                    }
                    proc_free(pp);
                    spinlock_release(&pp->lk);
                    spinlock_release(&wait_lock);
                    return pid;
                }
                spinlock_release(&pp->lk);
            }
        }
        
        if(!havekids) {
            spinlock_release(&wait_lock);
            return -1;
        }
        
        proc_sleep(p, &wait_lock);
    }
}

// 唤醒一个进程
static void proc_wakeup_one(proc_t* p)
{
    if(p->state == SLEEPING && p->sleep_space == p) {
        p->state = RUNNABLE;
    }
}

// 过继子进程
static void proc_reparent(proc_t* parent)
{
    proc_t* pp;
    
    for(pp = proc; pp < &proc[NPROC]; pp++) {
        if(pp->parent == parent) {
            pp->parent = proczero;
            proc_wakeup_one(proczero);
        }
    }
}

// 进程退出
void proc_exit(int exit_state)
{
    proc_t* p = myproc();
    
    if(p == proczero)
        panic("init exiting");
    
    spinlock_acquire(&wait_lock);
    
    proc_reparent(p);
    proc_wakeup_one(p->parent);
    
    spinlock_acquire(&p->lk);
    
    p->exit_state = exit_state;
    p->state = ZOMBIE;
    
    spinlock_release(&wait_lock);
    
    proc_sched();
    panic("zombie exit");
}

// 进程切换到调度器
void proc_sched()
{
    int origin;
    proc_t* p = myproc();
    
    if(!spinlock_holding(&p->lk))
        panic("sched p->lk");
    if(mycpu()->noff != 1)
        panic("sched locks");
    if(p->state == RUNNING)
        panic("sched running");
    if(intr_get())
        panic("sched interruptible");
    
    origin = mycpu()->origin;
    swtch(&p->ctx, &mycpu()->ctx);
    mycpu()->origin = origin;
}

// 调度器
void proc_scheduler()
{
    proc_t* p;
    cpu_t* c = mycpu();
    
    c->proc = 0;
    for(;;) {
        intr_on();
        
        for(p = proc; p < &proc[NPROC]; p++) {
            spinlock_acquire(&p->lk);
            if(p->state == RUNNABLE) {
                p->state = RUNNING;
                c->proc = p;
                swtch(&c->ctx, &p->ctx);
                c->proc = 0;
            }
            spinlock_release(&p->lk);
        }
    }
}

// 进程睡眠
void proc_sleep(void* sleep_space, spinlock_t* lk)
{
    proc_t* p = myproc();
    
    spinlock_acquire(&p->lk);
    spinlock_release(lk);
    
    p->sleep_space = sleep_space;
    p->state = SLEEPING;
    
    proc_sched();
    
    p->sleep_space = 0;
    
    spinlock_release(&p->lk);
    spinlock_acquire(lk);
}

// 唤醒所有睡眠进程
void proc_wakeup(void* sleep_space)
{
    proc_t* p;
    
    for(p = proc; p < &proc[NPROC]; p++) {
        if(p != myproc()) {
            spinlock_acquire(&p->lk);
            if(p->state == SLEEPING && p->sleep_space == sleep_space) {
                p->state = RUNNABLE;
            }
            spinlock_release(&p->lk);
        }
    }
}