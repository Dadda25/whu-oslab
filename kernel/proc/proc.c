#include "lib/print.h"
#include "lib/str.h"
#include "lib/lock.h"
#include "mem/pmem.h"
#include "mem/vmem.h"
#include "proc/cpu.h"
#include "proc/initcode.h"
#include "fs/fs.h"
#include "memlayout.h"
#include "riscv.h"

/*----------------外部空间------------------*/

// in trampoline.S
extern char trampoline[];

// in swtch.S
extern void swtch(context_t* old, context_t* new);

// in trap_user.c
extern void trap_user_return();

/*----------------本地变量------------------*/

// 进程定义相关
static proc_t* proczero;// 用户态下的第一个进程（指针）
proc_t proc[NPROC];     // 数组

// 进程ID号相关
int nextpid = 1;            // 下一个pid编号
struct spinlock pid_lock;   // 获取pid的锁

// wait的自旋锁，在wait()函数中使用
struct spinlock wait_lock;

// 获取新的进程id号（锁保护）
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

// 创建一个进程
proc_t* proc_alloc(void)
{
  proc_t *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    spinlock_acquire(&p->lk);
    if(p->state == UNUSED) {
      goto found;
    } else {
      spinlock_release(&p->lk);
    }
  }
  return 0;

found:
  p->pid = allocpid();
  p->state = RUNNABLE;

  // Allocate a trapframe page.
  if((p->tf = (struct trapframe *)pmem_alloc(false)) == 0){
    proc_free(p);
    spinlock_release(&p->lk);
    return 0;
  }

  // An empty user page table.
  p->pgtbl = proc_pgtbl_init((uint64)p->tf);
  if(p->pgtbl == 0){
    proc_free(p);
    spinlock_release(&p->lk);
    return 0;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->ctx, 0, sizeof(p->ctx));
  p->ctx.ra = (uint64)fork_return;
  p->ctx.sp = p->kstack + PGSIZE;

  return p;
}

// 释放一个进程
void proc_free(proc_t *p)
{
    if(p->tf)
    pmem_free((uint64)p->tf, false);
    p->tf = 0;
    if(p->pgtbl)

    uvm_destroy_pgtbl(p->pgtbl);
    
    p->pgtbl = 0;
    p->ustack_pages = 0;
    p->parent = 0;
    p->sleep_space = 0;
    p->killed = 0;
    p->xstate = 0;
    p->state = UNUSED;
}

// 初始化进程数组
void proc_init(void)
{
    proc_t *p;
  
    spinlock_init(&pid_lock, "nextpid");
    spinlock_init(&wait_lock, "wait_lock");
    for(p = proc; p < &proc[NPROC]; p++) {
        spinlock_init(&p->lk, "proc");
        p->state = UNUSED;
        p->kstack = KSTACK((int) (p - proc));
    }
}

// 获得一个初始化过的用户页表
// 完成了trapframe 和 trampoline 的映射
pgtbl_t proc_pgtbl_init(uint64 trapframe)
{
    pgtbl_t pagetable;

    // 建立一个空的页表
    pagetable = (pgtbl_t) pmem_alloc(false);
    if(pagetable == 0) return 0;
    memset(pagetable, 0, PGSIZE);

    // map the trampoline code (for system call return)
    // at the highest user virtual address.
    // only the supervisor uses it, on the way
    // to/from user space, so not PTE_U.
    vm_mappages(pagetable, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

    // map the trapframe page just below the trampoline page, for
    // trampoline.S.
    vm_mappages(pagetable, TRAPFRAME, (uint64)(trapframe), PGSIZE, PTE_R | PTE_W);

    return pagetable;
}

// 在内核中映射栈部分的内存
void proc_mapstacks(pgtbl_t kpgtbl)
{
    proc_t *p;
  
    for(p = proc; p < &proc[NPROC]; p++) {
        char *pa = pmem_alloc(false);
        if(pa == 0)
            panic("kalloc");
        uint64 va = KSTACK((int) (p - proc));
        vm_mappages(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
    }

    // char* pa = pmem_alloc(false);
    // uint64 va = KSTACK(0);
    // vm_mappages(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W); 
}

/*  第一个用户态进程的创建
    它的代码和数据位于initcode.h的initcode数组

    第一个进程的用户地址空间布局:
    trapoline   (1 page)
    trapframe   (1 page)
    ustack      (1 page)
    .......
                        <--heap_top
    code + data (? page) 可能随测试代码的页数而变化
    empty space (1 page) 最低的4096字节 不分配物理页，同时不可访问
*/
// 修复后的 proc_make_first 函数

void proc_make_first()
{
    struct proc *p;

    p = proc_alloc();
    proczero = p;
    proczero->pid = 0;

    proczero->kstack = KSTACK(0);  // 确保内核栈地址正确

    memset(proczero->tf, 0, sizeof(trapframe_t));

    printf("[proc_make_first] Starting...\n");
    
    // 映射代码（从 0x1000 开始）
    uint64 CODE_VA = PGSIZE;
    printf("[proc_make_first] Mapping code at 0x1000...\n");
    
    // 3 页代码
    char *mem0 = (char *)pmem_alloc(false);
    if (!mem0) panic("alloc page 0");
    memset(mem0, 0, PGSIZE);
    vm_mappages(proczero->pgtbl, CODE_VA, (uint64)mem0, PGSIZE, 
                PTE_W|PTE_R|PTE_X|PTE_U);
    memmove(mem0, initcode, 4096);
    printf("[proc_make_first] Page at 0x1000 done\n");
    
    char *mem1 = (char *)pmem_alloc(false);
    if (!mem1) panic("alloc page 1");
    memset(mem1, 0, PGSIZE);
    vm_mappages(proczero->pgtbl, CODE_VA + PGSIZE, (uint64)mem1, PGSIZE, 
                PTE_W|PTE_R|PTE_X|PTE_U);
    memmove(mem1, initcode + 4096, 4096);
    printf("[proc_make_first] Page at 0x2000 done\n");
    
    char *mem2 = (char *)pmem_alloc(false);
    if (!mem2) panic("alloc page 2");
    memset(mem2, 0, PGSIZE);
    vm_mappages(proczero->pgtbl, CODE_VA + 2 * PGSIZE, (uint64)mem2, PGSIZE, 
                PTE_W|PTE_R|PTE_X|PTE_U);
    memmove(mem2, initcode + 8192, 860);
    printf("[proc_make_first] Page at 0x3000 done\n");

    // ===== 关键修复：正确映射用户栈 =====
    // 用户栈在代码之后，从 0x4000 开始
    uint64 USTACK_VA = CODE_VA + 3 * PGSIZE;  // 0x4000
    
    char *ustack_mem = (char *)pmem_alloc(false);
    if (!ustack_mem) panic("alloc ustack");
    memset(ustack_mem, 0, PGSIZE);
    
    vm_mappages(proczero->pgtbl, USTACK_VA, (uint64)ustack_mem, PGSIZE, 
                PTE_R | PTE_W | PTE_U);
    
    printf("[proc_make_first] User stack mapped at 0x%x\n", USTACK_VA);
    
    // 更新进程信息
    proczero->ustack_pages = 1;
    proczero->heap_top = USTACK_VA + PGSIZE;  // heap 从栈之后开始
    
    printf("[proc_make_first] heap_top = 0x%x (%d)\n", 
           proczero->heap_top, proczero->heap_top);

    // ===== 设置 trapframe 中的 epc 和 sp =====
    proczero->tf->epc = CODE_VA;              // 从 0x1000 开始执行
    proczero->tf->sp = USTACK_VA + PGSIZE;    // 栈顶在 0x5000（栈向下增长）
    
    printf("[proc_make_first] epc = 0x%x, sp = 0x%x\n", 
           proczero->tf->epc, proczero->tf->sp);
    printf("[proc_make_first] Setup complete!\n\n");

    proczero->state = RUNNABLE;
    spinlock_release(&proczero->lk);
}

// 进程复制
// UNUSED -> RUNNABLE
int proc_fork() {
    int pid;
    proc_t *np;
    proc_t *p = myproc();

    // Allocate process.
    if((np = proc_alloc()) == 0){
        return -1;
    }

    // Copy user memory from parent to child.
    uvm_copy_pgtbl(p->pgtbl, np->pgtbl, p->heap_top, p->ustack_pages, 0);
    // 用户栈的映射在这里进行
    np->ustack_pages = p->ustack_pages;

    // copy saved user registers.
    *(np->tf) = *(p->tf);

    // Cause fork to return 0 in the child.
    np->tf->a0 = 0;

    // increment reference counts on open file descriptors.
    /*
    for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
        np->ofile[i] = filedup(p->ofile[i]);
    np->cwd = idup(p->cwd);*/

    // safestrcpy(np->name, p->name, sizeof(p->name));

    pid = np->pid;

    spinlock_release(&np->lk);

    spinlock_acquire(&wait_lock);
    np->parent = p;
    spinlock_release(&wait_lock);

    spinlock_acquire(&np->lk);
    np->state = RUNNABLE;
    spinlock_release(&np->lk);

    return pid;
}

// 进程放弃CPU的控制权
// RUNNING -> RUNNABLE
void proc_yield()
{
    struct proc *p = myproc();
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
  proc_t *pp;
  int havekids, pid;
  proc_t *p = myproc();

  spinlock_acquire(&wait_lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(pp = proc; pp < &proc[NPROC]; pp++){
      if(pp->parent == p){
        // make sure the child isn't still in exit() or swtch().
        spinlock_acquire(&pp->lk);

        havekids = 1;
        if(pp->state == ZOMBIE){
          // Found one.
          pid = pp->pid;
          if(addr != 0) {
            uvm_copyout(p->pgtbl, addr, (uint64)&pp->xstate, sizeof(pp->xstate));
          }
          proc_free(pp);
          spinlock_release(&pp->lk);
          spinlock_release(&wait_lock);
          return pid;
        }
        spinlock_release(&pp->lk);
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || proc_killed(p)){
      spinlock_release(&wait_lock);
      return -1;
    }
    
    // Wait for a child to exit.
    proc_sleep(p, &wait_lock);  //DOC: wait-sleep
  }
}

// 唤醒一个进程
static void proc_wakeup_one(proc_t* p)
{
    assert(spinlock_holding(&p->lk) == 0, "proc_wakeup_one: lock");
    if(p->state == SLEEPING && p->sleep_space == p) {
        p->state = RUNNABLE;
    }
}

// 父进程退出，子进程认proczero做父，因为它永不退出
static void proc_reparent(proc_t* parent)
{
  proc_t *pp;

  for(pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->parent == parent){
      pp->parent = proczero;
      proc_wakeup_one(proczero);
    }
  }
}

// 进程退出
void proc_exit(int exit_state)
{
    struct proc *p = myproc();

    if(p == proczero)
    panic("init exiting");

    // Close all open files.
    /*
    // 与文件相关的部分，现在还没有用：/
    for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
        struct file *f = p->ofile[fd];
        fileclose(f);
        p->ofile[fd] = 0;
    }
    }
    begin_op();
    iput(p->cwd);
    end_op();
    p->cwd = 0;
    */

    spinlock_acquire(&wait_lock);

    // Give any children to init.
    proc_reparent(p);

    // Parent might be sleeping in wait().

    proc_wakeup_one(p->parent);

    spinlock_acquire(&p->lk);

    p->xstate = exit_state;
    p->state = ZOMBIE;

    spinlock_release(&wait_lock);

    // Jump into the scheduler, never to return.
    proc_sched();
    panic("zombie exit");
}

// 杀死一个进程
int proc_kill(int pid)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++){
    spinlock_acquire(&p->lk);
    if(p->pid == pid){
      p->killed = true;
      if(p->state == SLEEPING){
        // Wake process from sleep().
        p->state = RUNNABLE;
      }
      spinlock_release(&p->lk);
      return 0;
    }
    spinlock_release(&p->lk);
  }
  return -1;
}

// 将进程修改为杀死状态（在遇见异常时使用）
void proc_setkilled(proc_t *proc)
{
    spinlock_acquire(&proc->lk);
    proc->killed = 1;
    spinlock_release(&proc->lk);
}

// 检查进程是否被杀死
bool proc_killed(proc_t *proc)
{
    bool k;

    spinlock_acquire(&proc->lk);
    k = proc->killed;
    spinlock_release(&proc->lk);
    return k;
}

// 进程切换到调度器
// ps: 调用者保证持有当前进程的锁
void proc_sched()
{
    int origin;
    struct proc *p = myproc();

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
    struct proc *p;
    struct cpu *c = mycpu();

    c->proc = 0;
    for(;;){
        // Avoid deadlock by ensuring that devices can interrupt.
        intr_on();

        for(p = proc; p < &proc[NPROC]; p++) {
            spinlock_acquire(&p->lk);
            if(p->state == RUNNABLE) {
                // Switch to chosen process.  It is the process's job
                // to release its lock and then reacquire it
                // before jumping back to us.
                p->state = RUNNING;
                c->proc = p;
                swtch(&c->ctx, &p->ctx);

                // Process is done running for now.
                // It should have changed its p->state before coming back.
                c->proc = 0;
            }
            spinlock_release(&p->lk);
        }
    }
}

// 进程睡眠在sleep_space
void proc_sleep(void* sleep_space, spinlock_t* lk)
{
    struct proc *p = myproc();

    // Must acquire p->lk in order to
    // sleep_spacege p->state and then call sched.
    // Once we hold p->lk, we can be
    // guaranteed that we won't miss any wakeup
    // (wakeup locks p->lk),
    // so it's okay to release lk.

    spinlock_acquire(&p->lk);  //DOC: sleeplock1
    spinlock_release(lk);

    // Go to sleep.
    p->sleep_space = sleep_space;
    p->state = SLEEPING;

    proc_sched();

    // Tidy up.
    p->sleep_space = 0;

    // Reacquire original lock.
    spinlock_release(&p->lk);
    spinlock_acquire(lk);
}

// 唤醒所有在sleep_space沉睡的进程
void proc_wakeup(void* sleep_space)
{
    struct proc *p;

    for(p = proc; p < &proc[NPROC]; p++) {
        if(p != myproc()){
            spinlock_acquire(&p->lk);
            if(p->state == SLEEPING && p->sleep_space == sleep_space) {
                p->state = RUNNABLE;
            }
            spinlock_release(&p->lk);
        }
    }
}