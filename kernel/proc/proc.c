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
static proc_t procs[NPROC];

// 第一个进程的指针
static proc_t* proczero;

// 在文件开头添加
static void proc_wakeup_one(proc_t* p);

// 全局的pid和保护它的锁 
static int global_pid = 1;
static spinlock_t lk_pid;

// 申请一个pid(锁保护)
static int alloc_pid()
{
    int tmp = 0;
    spinlock_acquire(&lk_pid);
    assert(global_pid >= 0, "alloc_pid: overflow");
    tmp = global_pid++;
    spinlock_release(&lk_pid);
    return tmp;
}

// 释放锁 + 调用 trap_user_return
static void fork_return()
{
    // 由于调度器中上了锁，所以这里需要解锁
    proc_t* p = myproc();
    printf("fork_return: pid=%d, releasing lock %p\n", p->pid, &p->lk);
    spinlock_release(&p->lk);
    trap_user_return();
}

// 返回一个未使用的进程空间
// 设置pid + 设置上下文中的ra和sp
// 申请tf和pgtbl使用的物理页
proc_t* proc_alloc()
{
    proc_t* p = NULL;
    
    // 查找一个UNUSED状态的进程槽
    for(int i = 0; i < NPROC; i++) {
        spinlock_acquire(&procs[i].lk);
        if(procs[i].state == UNUSED) {
            p = &procs[i];
            break;
        }
        spinlock_release(&procs[i].lk);
    }
    
    if(p == NULL) {
        return NULL;  // 没有空闲进程槽
    }
    
    // 分配pid
    p->pid = alloc_pid();
    p->state = RUNNABLE;  // 暂时设置为RUNNABLE，后续可能会修改
    
    // 分配trapframe物理页
    p->tf = (trapframe_t*)pmem_alloc(PMEM_KERNEL);
    if(p->tf == 0) {
        pmem_free((uint64)p, PMEM_KERNEL);  // 修改：添加第二个参数
        return 0;
    }
    
    // 分配页表
    p->pgtbl = proc_pgtbl_init((uint64)p->tf);
    if(p->pgtbl == 0) {
        pmem_free((uint64)p->tf, PMEM_KERNEL);  // 修改：添加第二个参数
        pmem_free((uint64)p, PMEM_KERNEL);      // 修改：添加第二个参数
        return 0;
    }
    
    // 设置内核栈地址
    p->kstack = KSTACK(p->pid);
    
    // 设置context，使得第一次调度到该进程时会执行fork_return
    memset(&p->ctx, 0, sizeof(context_t));
    p->ctx.ra = (uint64)fork_return;
    p->ctx.sp = p->kstack + PGSIZE;  // 栈顶
    
    return p;
}

// 释放一个进程空间
// 释放pgtbl的整个地址空间
// 释放mmap_region到仓库
// 设置其余各个字段为合适初始值
// tips: 调用者需持有p->lk
void proc_free(proc_t* p)
{
    assert(spinlock_holding(&p->lk), "proc_free: lock");
    
    // 释放trapframe
    if(p->tf)
        pmem_free((uint64)p->tf, PMEM_KERNEL);  // 修改：添加第二个参数
    
    // 释放页表及其映射的物理页
    if(p->pgtbl) {
        uvm_destroy_pgtbl(p->pgtbl);
        p->pgtbl = NULL;
    }
    
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
    // 初始化pid锁
    spinlock_init(&lk_pid, "pid");
    
    // 初始化所有进程槽
    for(int i = 0; i < NPROC; i++) {
        spinlock_init(&procs[i].lk, "proc");
        procs[i].state = UNUSED;
        procs[i].pid = 0;
        procs[i].parent = NULL;
        procs[i].exit_state = 0;
        procs[i].sleep_space = NULL;
        procs[i].pgtbl = NULL;
        procs[i].heap_top = 0;
        procs[i].ustack_pages = 0;
        procs[i].mmap = NULL;
        procs[i].tf = NULL;
        procs[i].kstack = 0;
    }
    
    printf("proc_init: process system initialized\n");
}

// 获得一个初始化过的用户页表
// 完成了trapframe 和 trampoline 的映射
pgtbl_t proc_pgtbl_init(uint64 trapframe)
{
    pgtbl_t pgtbl = (pgtbl_t)pmem_alloc(PMEM_KERNEL);
    if(pgtbl == 0)
        return 0;
    memset(pgtbl, 0, PGSIZE);
    
    // 映射trampoline页（内核和用户共享）
    // trampoline在内核中的物理地址就是其符号地址
    vm_mappages(pgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);
    
    // 映射trapframe页
    vm_mappages(pgtbl, TRAPFRAME, trapframe, PGSIZE, PTE_R | PTE_W);
    
    return pgtbl;
}

/*
    第一个用户态进程的创建
    它的代码和数据位于initcode.h的initcode数组

    第一个进程的用户地址空间布局:
    trapoline   (1 page)
    trapframe   (1 page)
    ustack      (1 page)
    .......
                        <--heap_top
    code + data (1 page)
    empty space (1 page) 最低的4096字节 不分配物理页，同时不可访问

    UNUSED -> RUNNABLE
*/
void proc_make_first()
{
    proczero = proc_alloc();
    if(proczero == NULL) {
        panic("proc_make_first: proc_alloc failed");
    }
    
    // 此时持有proczero的锁
    
    // 映射initcode到虚拟地址PGSIZE（跳过第一页）
    uint64 code_pa = (uint64)pmem_alloc(PMEM_USER);
    if(code_pa == 0) {
        panic("proc_make_first: pmem_alloc");
    }
    
    // 拷贝initcode到物理页
    memmove((void*)code_pa, initcode, initcode_len);
    
    // 建立映射
    vm_mappages(proczero->pgtbl, PGSIZE, code_pa, PGSIZE, PTE_R | PTE_W | PTE_X | PTE_U);
    
    // 映射用户栈（在代码页上方）
    uint64 stack_pa = (uint64)pmem_alloc(PMEM_USER);
    if(stack_pa == 0) {
        panic("proc_make_first: stack alloc failed");
    }
    
    vm_mappages(proczero->pgtbl, PGSIZE * 2, stack_pa, PGSIZE, PTE_R | PTE_W | PTE_U);
    
    proczero->ustack_pages = 1;
    proczero->heap_top = PGSIZE * 3;  // 堆顶在栈下方
    proczero->parent = NULL;
    proczero->mmap = NULL;
    
    // 设置trapframe
    memset(proczero->tf, 0, sizeof(trapframe_t));
    proczero->tf->epc = PGSIZE;  // 用户程序入口
    proczero->tf->sp = PGSIZE * 3;  // 用户栈顶（栈向下增长）
    
    proczero->state = RUNNABLE;
    
    spinlock_release(&proczero->lk);
    
    printf("proc_make_first: first process created (pid=%d)\n", proczero->pid);
}

// 进程复制
// UNUSED -> RUNNABLE
int proc_fork()
{
    proc_t* parent = myproc();
    
    // 分配新进程
    proc_t* child = proc_alloc();
    if(child == NULL) {
        return -1;
    }
    
    // 此时持有child的锁
    
    // 拷贝用户页表
    uvm_copy_pgtbl(child->pgtbl, parent->pgtbl, parent->heap_top, 
                   parent->ustack_pages, parent->mmap);
    
    // 拷贝进程状态
    child->heap_top = parent->heap_top;
    child->ustack_pages = parent->ustack_pages;
    
    // 深拷贝 mmap 链表
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
    memmove(child->tf, parent->tf, sizeof(trapframe_t));
    
    // 子进程fork返回0
    child->tf->a0 = 0;
    
    // 设置父子关系
    child->parent = parent;
    
    // 子进程状态设置为RUNNABLE
    child->state = RUNNABLE;
    
    int pid = child->pid;
    
    spinlock_release(&child->lk);
    
    return pid;  // 父进程返回子进程pid
}

// 进程放弃CPU的控制权
// RUNNING -> RUNNABLE
void proc_yield()
{
    proc_t* p = myproc();
    spinlock_acquire(&p->lk);
    p->state = RUNNABLE;
    proc_sched();
    spinlock_release(&p->lk);
}

// 等待一个子进程进入 ZOMBIE 状态
// 将退出的子进程的exit_state放入用户给的地址 addr
// 成功返回子进程pid，失败返回-1
int proc_wait(uint64 addr)
{
    proc_t* p = myproc();
    int have_children;
    int pid;
    
    spinlock_acquire(&p->lk);
    
    for(;;) {
        have_children = 0;
        
        // 遍历所有进程，查找子进程
        for(int i = 0; i < NPROC; i++) {
            spinlock_acquire(&procs[i].lk);
            
            if(procs[i].parent == p) {
                have_children = 1;
                
                if(procs[i].state == ZOMBIE) {
                    // 找到一个僵尸子进程
                    pid = procs[i].pid;
                    
                    // 将exit_state拷贝到用户空间
                    if(addr != 0) {
                        uvm_copyout(p->pgtbl, addr, (uint64)&procs[i].exit_state, sizeof(int));
                    }
                    
                    // 释放子进程资源
                    proc_free(&procs[i]);
                    spinlock_release(&procs[i].lk);
                    spinlock_release(&p->lk);
                    
                    return pid;
                }
            }
            
            spinlock_release(&procs[i].lk);
        }
        
        // 没有子进程
        if(!have_children) {
            spinlock_release(&p->lk);
            return -1;
        }
        
        // 有子进程但还没退出，睡眠等待
        proc_sleep(p, &p->lk);
    }
}

// 父进程退出，子进程认proczero做父，因为它永不退出
static void proc_reparent(proc_t* parent)
{
    for(int i = 0; i < NPROC; i++) {
        spinlock_acquire(&procs[i].lk);
        if(procs[i].parent == parent) {
            procs[i].parent = proczero;
            // 如果子进程已经是僵尸，唤醒proczero
            if(procs[i].state == ZOMBIE) {
                proc_wakeup_one(proczero);
            }
        }
        spinlock_release(&procs[i].lk);
    }
}

// 唤醒一个进程
static void proc_wakeup_one(proc_t* p)
{
    assert(spinlock_holding(&p->lk), "proc_wakeup_one: lock");
    if(p->state == SLEEPING && p->sleep_space == p) {
        p->state = RUNNABLE;
    }
}

// 进程退出
void proc_exit(int exit_state)
{
    proc_t* p = myproc();
    
    assert(p != proczero, "proc_exit: proczero cannot exit");
    
    spinlock_acquire(&p->lk);
    
    // 保存退出状态
    p->exit_state = exit_state;
    
    // 将子进程过继给proczero
    proc_reparent(p);
    
    // 唤醒父进程（如果父进程在wait中睡眠）
    if(p->parent) {
        spinlock_acquire(&p->parent->lk);
        // 只需要将父进程标记为可唤醒，不要在持有锁时调用proc_wakeup
        if(p->parent->state == SLEEPING && p->parent->sleep_space == p->parent) {
            p->parent->state = RUNNABLE;
        }
        spinlock_release(&p->parent->lk);
    }
    
    // 设置为ZOMBIE状态
    p->state = ZOMBIE;
    
    // 切换到调度器，不再返回
    proc_sched();
    
    panic("proc_exit: unreachable");
}

// 进程切换到调度器
// ps: 调用者保证持有当前进程的锁
void proc_sched() {
    proc_t* p = myproc();
    
    // 检查是否持有锁
    if(!spinlock_holding(&p->lk))
        panic("proc_sched: not holding lock");
    
    // 检查中断嵌套深度（应该只有进程锁这一个锁）
    if(mycpu()->noff != 1)
        panic("proc_sched: locks");
    
    // 检查进程状态（不能在 RUNNING 状态下切换）
    if(p->state == RUNNING)
        panic("proc_sched: running");
    
    // 保存中断状态
    int intena = mycpu()->origin;
    
    // 切换到调度器
    swtch(&p->ctx, &mycpu()->ctx);
    
    // 恢复中断状态
    mycpu()->origin = intena;
}

// 调度器
void proc_scheduler() {
    cpu_t* c = mycpu();
    c->proc = NULL;
    
    for(;;) {
        // 开中断，允许设备中断
        intr_on();
        
        // 遍历进程表，寻找RUNNABLE进程
        for(int i = 0; i < NPROC; i++) {
            spinlock_acquire(&procs[i].lk);
            
            if(procs[i].state == RUNNABLE) {
                procs[i].state = RUNNING;
                c->proc = &procs[i];
                
                // 切换到该进程（持有锁）
                swtch(&c->ctx, &procs[i].ctx);
                c->proc = NULL;
            }
            
            // 释放锁
            spinlock_release(&procs[i].lk);
        }
    }
}

// 进程睡眠在sleep_space
void proc_sleep(void* sleep_space, spinlock_t* lk)
{
    proc_t* p = myproc();
    
    // 必须持有进程锁
    assert(lk != &p->lk, "proc_sleep: cannot sleep on proc lock");
    
    spinlock_acquire(&p->lk);
    spinlock_release(lk);
    
    // 设置睡眠状态
    p->sleep_space = sleep_space;
    p->state = SLEEPING;
    
    // 切换到调度器
    proc_sched();
    
    // 被唤醒后清除sleep_space
    p->sleep_space = NULL;
    
    spinlock_release(&p->lk);
    spinlock_acquire(lk);
}

// 唤醒所有在sleep_space沉睡的进程
void proc_wakeup(void* sleep_space)
{
    for(int i = 0; i < NPROC; i++) {
        if(&procs[i] != myproc()) {
            spinlock_acquire(&procs[i].lk);
            if(procs[i].state == SLEEPING && procs[i].sleep_space == sleep_space) {
                procs[i].state = RUNNABLE;
            }
            spinlock_release(&procs[i].lk);
        }
    }
}