#include "lib/print.h"
#include "lib/str.h"
#include "mem/pmem.h"
#include "mem/vmem.h"
#include "proc/proc.h"
#include "common.h"
#include "memlayout.h"
#include "riscv.h"

pgtbl_t kernel_pagetable; // 页表

extern char etext[]; // kernel.ld设置的etext段

extern char trampoline[]; // trampoline.S

// 递归输出页表的对应内容
// ps: 顶级页表level = 0, level = 3 说明是页表管理的物理页
static void vm_print_helper(pgtbl_t pgtbl, int level){
    for(int i=0; i<512; i++){
        pte_t *pte = &pgtbl[i];
        pgtbl_t pa = (pgtbl_t)PTE_TO_PA(*pte);
        if(pte && pa){
            for(int k=0; k<level; k++) printf("    ");
            printf("%d: pte %p, pa %p\n", i, pte, (PTE_TO_PA(*pte)));
            if(level < 2)
                vm_print_helper(pa, level+1); // 页表项 < 2
        }
    }
}

void   vm_print(pgtbl_t pgtbl){
    vm_print_helper(pgtbl, 0);
}

void vm_getpteprint(pgtbl_t pgtbl, uint64 va, bool alloc){
    for(int level = 2; level > 0; level--) {
        pte_t *pte = &pgtbl[VA_TO_VPN(va, level)];
        printf("pte debug %p *:%p\n",pte,*pte);
        if(*pte & PTE_V) {
            pgtbl = (pgtbl_t)PTE_TO_PA(*pte);
        } else {
            if(!alloc || (pgtbl = (pte_t*)pmem_alloc(true)) == 0)
                return;
            memset(pgtbl, 0, PGSIZE);
            *pte = PA_TO_PTE(pgtbl) | PTE_V;
        }
  }
  return;
}

pte_t* vm_getpte(pgtbl_t pgtbl, uint64 va, bool alloc){
    if(pgtbl == NULL)
        pgtbl = kernel_pagetable;
    for(int level = 2; level > 0; level--) {
        pte_t *pte = &pgtbl[VA_TO_VPN(va, level)];
        if(*pte & PTE_V) {
            pgtbl = (pgtbl_t)PTE_TO_PA(*pte);
        } else {
            if(!alloc || (pgtbl = (pte_t*)pmem_alloc(true)) == 0)
                return 0;
            memset(pgtbl, 0, PGSIZE);
            *pte = PA_TO_PTE(pgtbl) | PTE_V;
        }
  }
  return &pgtbl[VA_TO_VPN(va, 0)];
}

uint64 vm_getpakernel(pgtbl_t pgtbl, uint64 va){
    if(va >= VA_MAX) return 0;

    pte_t* pte = vm_getpte(pgtbl, va, 0);
    if(pte == 0)
        return 0;
    else if((*pte & PTE_V) == 0 )
        return 0;
    
    uint64 pa = PTE_TO_PA(*pte);
    return pa;
}

// 同xv6中的walkaddr，返回虚拟地址在页表中对应的物理地址
// 如果没有映射或不可用，返回0
// 仅能用于查看用户页
uint64 vm_getpa(pgtbl_t pgtbl, uint64 va){
    if(va >= VA_MAX) return 0;

    pte_t* pte = vm_getpte(pgtbl, va, 0);
    if(pte == 0)
        return 0;
    else if((*pte & PTE_V) == 0 || (*pte & PTE_U) == 0)
        return 0;
    
    uint64 pa = PTE_TO_PA(*pte);
    return pa;
}

void   vm_mappages(pgtbl_t pgtbl, uint64 va, uint64 pa, uint64 len, int perm){
    uint64 a, last;
    pte_t *pte;

    if(len == 0)
        panic("mappages: len");

    a = PG_ROUND_DOWN(va);
    last = PG_ROUND_DOWN(va + len - 1);
    for(;;){
        if((pte = vm_getpte(pgtbl, a, 1)) == 0)
            panic("mappages: pte==0");
        if(*pte & PTE_V)
            panic("mappages: remap");
        *pte = PA_TO_PTE(pa) | perm | PTE_V;
        if(a == last) break;
        a += PGSIZE;
        pa += PGSIZE;
    }
}

void   vm_unmappages(pgtbl_t pgtbl, uint64 va, uint64 len, bool freeit){
    uint64 a;
    pte_t *pte;

    if((va % PGSIZE) != 0)
        panic("uvmunmap: not aligned");

    int len_rounded = PG_ROUND_UP(len);
    for(a = va; a < va + len_rounded; a += PGSIZE){
        if((pte = vm_getpte(pgtbl, a, 0)) == 0)
            panic("uvmunmap: vm_getpte");
        if((*pte & PTE_V) == 0)
            panic("uvmunmap: not mapped");
        if(PTE_FLAGS(*pte) == PTE_V)
            panic("uvmunmap: not a leaf");
        if(freeit){
            uint64 pa = PTE_TO_PA(*pte);
            pmem_free_auto(pa);
        }
        *pte = 0;
    }
}

// 创建页表并且提前分配
pgtbl_t kvm_create(){
    pgtbl_t kpgtbl;

    kpgtbl = (pgtbl_t) pmem_alloc(true);
    memset(kpgtbl, 0, PGSIZE);
    
    // UART 寄存器
    vm_mappages(kpgtbl, UART_BASE, UART_BASE, PGSIZE, PTE_R | PTE_W);

    // Virtio，即磁盘管理
    vm_mappages(kpgtbl, VIRTIO_BASE, VIRTIO_BASE, PGSIZE, PTE_R | PTE_W);

    // CLINT
    vm_mappages(kpgtbl, CLINT_BASE, CLINT_BASE, PGSIZE, PTE_R | PTE_W);

    // PLIC
    vm_mappages(kpgtbl, PLIC_BASE, PLIC_BASE, 0x400000, PTE_R | PTE_W);

    // 内核代码段KERNEL
    vm_mappages(kpgtbl, KERNEL_BASE, KERNEL_BASE, (uint64)etext-KERNEL_BASE, PTE_R | PTE_X);

    // 内核数据区KERNEL_DATA
    vm_mappages(kpgtbl, (uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);

    // trampoline，用于trap中虚拟地址的映射
    vm_mappages(kpgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);
    // printf("Trampoline address: va: %x, pa: %x\n", TRAMPOLINE, (uint64)trampoline);

    // 为每个进程创建一个栈
    proc_mapstacks(kpgtbl);

    return kpgtbl;
}

void   kvm_init(){
    kernel_pagetable = kvm_create();
}

void   kvm_inithart(){
    // wait for any previous writes to the page table memory to finish.
    sfence_vma();

    w_satp(MAKE_SATP(kernel_pagetable));

    // flush stale entries from the TLB.
    sfence_vma();
}
