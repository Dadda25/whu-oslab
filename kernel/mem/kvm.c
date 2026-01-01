// kernel virtual memory management

#include "mem/pmem.h"
#include "mem/vmem.h"
#include "lib/print.h"
#include "lib/str.h"
#include "riscv.h"
#include "memlayout.h"
#include "common.h"

extern char trampoline[]; // in trampoline.S

static pgtbl_t kernel_pgtbl; // 内核页表


// 根据pagetable,找到va对应的pte
// 若设置alloc=true 则在PTE无效时尝试申请一个物理页
// 成功返回PTE, 失败返回NULL
// 提示：使用 VA_TO_VPN PTE_TO_PA PA_TO_PTE
pte_t* vm_getpte(pgtbl_t pgtbl, uint64 va, bool alloc)
{
    // 三级页表遍历
    for(int level = 2; level > 0; level--) {
        uint64 vpn = VA_TO_VPN(va, level);
        pte_t* pte = &pgtbl[vpn];
        
        if((*pte) & PTE_V) {
            // PTE有效，获取下一级页表
            pgtbl = (pgtbl_t)PTE_TO_PA(*pte);
        } else {
            // PTE无效
            if(!alloc) {
                return NULL;
            }
            // 申请新的页表页
            pgtbl = (pgtbl_t)pmem_alloc(true);
            if(pgtbl == NULL) {
                return NULL;
            }
            // 设置PTE指向新页表，只设置V标志（页表页）
            *pte = PA_TO_PTE((uint64)pgtbl) | PTE_V;
        }
    }
    
    // 返回level 0的PTE
    uint64 vpn = VA_TO_VPN(va, 0);
    return &pgtbl[vpn];
}

// 在pgtbl中建立 [va, va + len) -> [pa, pa + len) 的映射
// 本质是找到va在页表对应位置的pte并修改它
// 检查: va pa 应当是 page-aligned, len(字节数) > 0, va + len <= VA_MAX
// 注意: perm 应该如何使用
void vm_mappages(pgtbl_t pgtbl, uint64 va, uint64 pa, uint64 len, int perm)
{
    // 参数检查
    assert(va % PGSIZE == 0, "vm_mappages: va not aligned");
    assert(pa % PGSIZE == 0, "vm_mappages: pa not aligned");
    assert(len > 0, "vm_mappages: len <= 0");
    assert(va + len <= VA_MAX, "vm_mappages: va + len > VA_MAX");
    
    uint64 va_start = va;
    uint64 va_end = va + len;
    uint64 pa_current = pa;
    
    for(uint64 va_current = va_start; va_current < va_end; va_current += PGSIZE, pa_current += PGSIZE) {
        pte_t* pte = vm_getpte(pgtbl, va_current, true);
        assert(pte != NULL, "vm_mappages: vm_getpte failed");
        assert(!((*pte) & PTE_V), "vm_mappages: remap");
        
        // 设置PTE：物理地址 + 权限标志 + V标志
        *pte = PA_TO_PTE(pa_current) | perm | PTE_V;
    }
}

// 解除pgtbl中[va, va+len)区域的映射
// 如果freeit == true则释放对应物理页, 默认是用户的物理页
void vm_unmappages(pgtbl_t pgtbl, uint64 va, uint64 len, bool freeit)
{
    assert(va % PGSIZE == 0, "vm_unmappages: va not aligned");
    assert(len > 0, "vm_unmappages: len <= 0");
    
    uint64 va_start = va;
    uint64 va_end = va + len;
    
    for(uint64 va_current = va_start; va_current < va_end; va_current += PGSIZE) {
        pte_t* pte = vm_getpte(pgtbl, va_current, false);
        if(pte == NULL || !((*pte) & PTE_V)) {
            continue;
        }
        
        // 如果需要释放物理页
        if(freeit) {
            uint64 pa = PTE_TO_PA(*pte);
            pmem_free(pa, false);
        }
        
        // 清除PTE
        *pte = 0;
    }
}

// 填充kernel_pgtbl
// 完成 UART CLINT PLIC 内核代码区 内核数据区 可分配区域 trampoline kstack 的映射
void kvm_init()
{
    // 申请内核页表
    kernel_pgtbl = (pgtbl_t)pmem_alloc(true);
    
    // UART 映射 (RW)
    vm_mappages(kernel_pgtbl, UART_BASE, UART_BASE, PGSIZE, PTE_R | PTE_W);
    
    // CLINT 映射 (RW)
    vm_mappages(kernel_pgtbl, CLINT_BASE, CLINT_BASE, 0x10000, PTE_R | PTE_W);
    
    // PLIC 映射 (RW)
    vm_mappages(kernel_pgtbl, PLIC_BASE, PLIC_BASE, 0x400000, PTE_R | PTE_W);
    
    // 内核代码区和数据区映射 (RWX)
    // 从KERNEL_BASE到KERNEL_DATA是代码区，从KERNEL_DATA到ALLOC_BEGIN是数据区
    vm_mappages(kernel_pgtbl, KERNEL_BASE, KERNEL_BASE, 
                (uint64)ALLOC_BEGIN - KERNEL_BASE, PTE_R | PTE_W | PTE_X);
    
    // 可分配区域映射 (RW)
    vm_mappages(kernel_pgtbl, (uint64)ALLOC_BEGIN, (uint64)ALLOC_BEGIN,
                (uint64)ALLOC_END - (uint64)ALLOC_BEGIN, PTE_R | PTE_W);
    
    // trampoline 映射 (RX)
    vm_mappages(kernel_pgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);
    
    // kstack 映射 - 这里假设最多支持8个CPU，每个CPU一个栈
    // 每个栈大小为PGSIZE，栈之间有guard page
    for(int i = 0; i < 8; i++) {
        uint64 kstack_va = KSTACK(i);
        uint64 kstack_pa = (uint64)pmem_alloc(true);
        vm_mappages(kernel_pgtbl, kstack_va, kstack_pa, PGSIZE, PTE_R | PTE_W);
    }
    
    printf("kvm_init: kernel page table initialized\n");
}

// 使用新的页表，刷新TLB
void kvm_inithart()
{
    w_satp(MAKE_SATP(kernel_pgtbl));
    sfence_vma();
}

// for debug
// 输出页表内容
void vm_print(pgtbl_t pgtbl)
{
    // 顶级页表，次级页表，低级页表
    pgtbl_t pgtbl_2 = pgtbl, pgtbl_1 = NULL, pgtbl_0 = NULL;
    pte_t pte;

    printf("level-2 pgtbl: pa = %p\n", pgtbl_2);
    for(int i = 0; i < PGSIZE / sizeof(pte_t); i++) 
    {
        pte = pgtbl_2[i];
        if(!((pte) & PTE_V)) continue;
        assert(PTE_CHECK(pte), "vm_print: pte check fail (1)");
        pgtbl_1 = (pgtbl_t)PTE_TO_PA(pte);
        printf(".. level-1 pgtbl %d: pa = %p\n", i, pgtbl_1);
        
        for(int j = 0; j < PGSIZE / sizeof(pte_t); j++)
        {
            pte = pgtbl_1[j];
            if(!((pte) & PTE_V)) continue;
            assert(PTE_CHECK(pte), "vm_print: pte check fail (2)");
            pgtbl_0 = (pgtbl_t)PTE_TO_PA(pte);
            printf(".. .. level-0 pgtbl %d: pa = %p\n", j, pgtbl_0);

            for(int k = 0; k < PGSIZE / sizeof(pte_t); k++) 
            {
                pte = pgtbl_0[k];
                if(!((pte) & PTE_V)) continue;
                assert(!PTE_CHECK(pte), "vm_print: pte check fail (3)");
                printf(".. .. .. physical page %d: pa = %p flags = %d\n", k, (uint64)PTE_TO_PA(pte), (int)PTE_FLAGS(pte));                
            }
        }
    }
}