#include "lib/print.h"
#include "lib/str.h"
#include "mem/pmem.h"
#include "mem/vmem.h"
#include "proc/cpu.h"
#include "proc/initcode.h"
#include "memlayout.h"
#include "riscv.h"

// in trampoline.S
extern char trampoline[];

// in swtch.S
extern void swtch(context_t* old, context_t* new);

// in trap_user.c
extern void trap_user_return();

// 第一个进程
static proc_t proczero;

// 在内核中映射栈部分的内存
void proc_mapstacks(pgtbl_t kpgtbl){
    char* pa = pmem_alloc(false);
    uint64 va = KSTACK(0);
    vm_mappages(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W); 
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
*/
void proc_make_first()
{   
    // pid 设置
    proczero.pid = 0;

    // pagetable 初始化
    uint64 trapframe_page = (uint64)pmem_alloc(false);
    proczero.pgtbl = proc_pgtbl_init(trapframe_page);

    // ustack 映射 + 设置 ustack_pages
    // proc_mapstacks已经完成了栈的设置和映射
    proczero.kstack = KSTACK(0);
    uint64 ustack_phys = (uint64)pmem_alloc(true);
    vm_mappages(proczero.pgtbl, proczero.kstack - PGSIZE, ustack_phys, PGSIZE, 
                PTE_R | PTE_W | PTE_U);
    
    // data + code 映射
    assert(initcode_len <= PGSIZE, "proc_make_first: initcode too big\n");
    char *mem = (char *)pmem_alloc(false);
    memset(mem, 0, PGSIZE);
    vm_mappages(proczero.pgtbl, 0, (uint64)mem, PGSIZE, PTE_W|PTE_R|PTE_X|PTE_U);
    memmove(mem, initcode, initcode_len);
    proczero.ustack_pages = 1;

    // 设置 heap_top
    proczero.heap_top = (uint64)initcode + PGSIZE;

    // tf字段设置
    proczero.tf = (struct trapframe *)trapframe_page;
    memset(proczero.tf, 0, sizeof(*proczero.tf));
    // 设置用户态返回时的关键寄存器
    proczero.tf->epc = 0; // 程序计数器，从虚拟地址0开始执行initcode
    proczero.tf->sp = PGSIZE; // 用户栈指针，设置在用户空间顶部

    // 内核字段设置
    memset(&proczero.ctx, 0, sizeof(&proczero.ctx));
    proczero.ctx.ra = (uint64)trap_user_return;
    proczero.ctx.sp = proczero.kstack + PGSIZE;

    // 上下文切换
    mycpu()->proc = &proczero;
    swtch(&mycpu()->ctx, &proczero.ctx);
}