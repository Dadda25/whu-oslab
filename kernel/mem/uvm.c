#include "mem/mmap.h"
#include "mem/pmem.h"
#include "mem/vmem.h"
#include "proc/cpu.h"
#include "lib/print.h"
#include "lib/str.h"
#include "memlayout.h"
#include "common.h"
#include "riscv.h"

// 连续虚拟空间的复制(在uvm_copy_pgtbl中使用)
static void copy_range(pgtbl_t old, pgtbl_t new, uint64 begin, uint64 end)
{
    uint64 va, pa, page;
    int flags;
    pte_t* pte;

    for(va = begin; va < end; va += PGSIZE)
    {
        pte = vm_getpte(old, va, false);
        
        if(pte == NULL || !((*pte) & PTE_V)) {
            continue;  // 跳过未映射的页
        }
        
        pa = (uint64)PTE_TO_PA(*pte);
        flags = (int)PTE_FLAGS(*pte);

        page = (uint64)pmem_alloc(false);
        if(page == 0) {
            panic("copy_range: pmem_alloc failed");
        }
        memmove((char*)page, (const char*)pa, PGSIZE);
        vm_mappages(new, va, page, PGSIZE, flags);
    }
}

// 递归释放 页表占用的物理页 和 页表管理的物理页
// ps: 顶级页表level = 3, level = 0 说明是页表管理的物理页
static void destroy_pgtbl(pgtbl_t pgtbl, uint32 level)
{
    if(level == 0) return;
    
    // 遍历当前级别的所有PTE
    for(int i = 0; i < PGSIZE / sizeof(pte_t); i++) {
        pte_t pte = pgtbl[i];
        if(pte & PTE_V) {
            if(level > 1) {
                // 这是一个指向下一级页表的PTE
                pgtbl_t next_pgtbl = (pgtbl_t)PTE_TO_PA(pte);
                destroy_pgtbl(next_pgtbl, level - 1);
                pmem_free((uint64)next_pgtbl, PMEM_KERNEL);
            } else {
                // level == 1，这是最后一级页表，释放物理页
                uint64 pa = PTE_TO_PA(pte);
                pmem_free(pa, PMEM_USER);
            }
        }
    }
}

// 页表销毁：trapframe 和 trampoline 单独处理
void uvm_destroy_pgtbl(pgtbl_t pgtbl)
{
    // 先解除trapframe和trampoline的映射（不释放物理页）
    vm_unmappages(pgtbl, TRAPFRAME, PGSIZE, false);
    vm_unmappages(pgtbl, TRAMPOLINE, PGSIZE, false);
    
    // 递归销毁页表
    destroy_pgtbl(pgtbl, 3);
    
    // 释放顶级页表本身
    pmem_free((uint64)pgtbl, PMEM_KERNEL);
}

// 拷贝页表 (拷贝并不包括trapframe 和 trampoline)
void uvm_copy_pgtbl(pgtbl_t old, pgtbl_t new, uint64 heap_top, uint32 ustack_pages, mmap_region_t* mmap)
{
    /* step-1: 从 PGSIZE 到 heap_top (代码段 + 堆) */
    if(heap_top > PGSIZE) {
        copy_range(old, new, PGSIZE, PG_ROUND_UP(heap_top));
    }

    /* step-2: 用户栈 */
    if(ustack_pages > 0) {
        uint64 stack_top = TRAPFRAME;
        uint64 stack_bottom = stack_top - ustack_pages * PGSIZE;
        copy_range(old, new, stack_bottom, stack_top);
    }

    /* step-3: mmap_region */
    mmap_region_t* tmp = mmap;
    while(tmp != NULL) {
        uint64 mmap_begin = tmp->begin;
        uint64 mmap_end = tmp->begin + tmp->npages * PGSIZE;
        copy_range(old, new, mmap_begin, mmap_end);
        tmp = tmp->next;
    }
}

// 在用户页表和进程mmap链里 新增mmap区域 [begin, begin + npages * PGSIZE)
// 页面权限为perm
void uvm_mmap(uint64 begin, uint32 npages, int perm)
{
    if(npages == 0) return;
    assert(begin % PGSIZE == 0, "uvm_mmap: begin not aligned");

    proc_t* p = myproc();
    
    /* 修改 mmap 链 */
    mmap_region_t* new_region = mmap_region_alloc();
    new_region->begin = begin;
    new_region->npages = npages;
    new_region->next = NULL;
    
    if(p->mmap == NULL) {
        p->mmap = new_region;
    } else {
        mmap_region_t* prev = NULL;
        mmap_region_t* curr = p->mmap;
        
        while(curr != NULL && curr->begin < begin) {
            prev = curr;
            curr = curr->next;
        }
        
        if(prev == NULL) {
            new_region->next = p->mmap;
            p->mmap = new_region;
        } else {
            new_region->next = curr;
            prev->next = new_region;
        }
    }

    /* 修改页表 */
    for(uint32 i = 0; i < npages; i++) {
        uint64 va = begin + i * PGSIZE;
        uint64 pa = (uint64)pmem_alloc(PMEM_USER);
        vm_mappages(p->pgtbl, va, pa, PGSIZE, perm);
    }
}

// 在用户页表和进程mmap链里释放mmap区域 [begin, begin + npages * PGSIZE)
void uvm_munmap(uint64 begin, uint32 npages)
{
    if(npages == 0) return;
    assert(begin % PGSIZE == 0, "uvm_munmap: begin not aligned");

    proc_t* p = myproc();
    uint64 end = begin + npages * PGSIZE;
    
    /* 处理 mmap 链 */
    mmap_region_t* prev = NULL;
    mmap_region_t* curr = p->mmap;
    
    while(curr != NULL) {
        uint64 curr_begin = curr->begin;
        uint64 curr_end = curr->begin + curr->npages * PGSIZE;
        
        if(curr_end <= begin || curr_begin >= end) {
            prev = curr;
            curr = curr->next;
            continue;
        }
        
        if(begin <= curr_begin && end >= curr_end) {
            if(prev == NULL) {
                p->mmap = curr->next;
            } else {
                prev->next = curr->next;
            }
            mmap_region_t* to_free = curr;
            curr = curr->next;
            mmap_region_free(to_free);
            continue;
        }
        
        if(begin <= curr_begin && end < curr_end) {
            uint32 removed_pages = (end - curr_begin) / PGSIZE;
            curr->begin = end;
            curr->npages -= removed_pages;
            prev = curr;
            curr = curr->next;
            continue;
        }
        
        if(begin > curr_begin && end >= curr_end) {
            uint32 removed_pages = (curr_end - begin) / PGSIZE;
            curr->npages -= removed_pages;
            prev = curr;
            curr = curr->next;
            continue;
        }
        
        if(begin > curr_begin && end < curr_end) {
            mmap_region_t* new_region = mmap_region_alloc();
            new_region->begin = end;
            new_region->npages = (curr_end - end) / PGSIZE;
            new_region->next = curr->next;
            
            curr->npages = (begin - curr_begin) / PGSIZE;
            curr->next = new_region;
            
            prev = new_region;
            curr = new_region->next;
            continue;
        }
        
        prev = curr;
        curr = curr->next;
    }

    /* 页表释放 */
    vm_unmappages(p->pgtbl, begin, npages * PGSIZE, true);
}

// 用户堆空间增加
uint64 uvm_heap_grow(pgtbl_t pgtbl, uint64 heap_top, uint32 len)
{
    uint64 new_heap_top = heap_top + len;
    uint64 heap_top_aligned = PG_ROUND_UP(heap_top);
    uint64 new_heap_top_aligned = PG_ROUND_UP(new_heap_top);
    
    if(new_heap_top_aligned >= TRAPFRAME - 256 * PGSIZE) {
        return -1;
    }
    
    if(new_heap_top_aligned > heap_top_aligned) {
        uint64 npages = (new_heap_top_aligned - heap_top_aligned) / PGSIZE;
        for(uint64 i = 0; i < npages; i++) {
            uint64 va = heap_top_aligned + i * PGSIZE;
            uint64 pa = (uint64)pmem_alloc(PMEM_USER);
            vm_mappages(pgtbl, va, pa, PGSIZE, PTE_R | PTE_W | PTE_U);
        }
    }

    return new_heap_top;
}

// 用户堆空间减少
uint64 uvm_heap_ungrow(pgtbl_t pgtbl, uint64 heap_top, uint32 len)
{
    if(len >= heap_top) {
        return 0;
    }
    
    uint64 new_heap_top = heap_top - len;
    uint64 heap_top_aligned = PG_ROUND_UP(heap_top);
    uint64 new_heap_top_aligned = PG_ROUND_UP(new_heap_top);
    
    if(new_heap_top_aligned < heap_top_aligned) {
        uint64 npages = (heap_top_aligned - new_heap_top_aligned) / PGSIZE;
        vm_unmappages(pgtbl, new_heap_top_aligned, npages * PGSIZE, true);
    }

    return new_heap_top;
}

// 其他函数保持不变...
void uvm_copyin(pgtbl_t pgtbl, uint64 dst, uint64 src, uint32 len)
{
    char* dst_ptr = (char*)dst;
    uint64 src_va = src;
    uint32 remaining = len;
    
    while(remaining > 0) {
        uint64 src_page_base = PG_ROUND_DOWN(src_va);
        uint64 offset_in_page = src_va - src_page_base;
        uint32 copy_len = PGSIZE - offset_in_page;
        if(copy_len > remaining) {
            copy_len = remaining;
        }
        
        pte_t* pte = vm_getpte(pgtbl, src_va, false);
        assert(pte != NULL && ((*pte) & PTE_V), "uvm_copyin: invalid address");
        
        uint64 pa = PTE_TO_PA(*pte);
        char* src_ptr = (char*)(pa + offset_in_page);
        
        memmove(dst_ptr, src_ptr, copy_len);
        
        dst_ptr += copy_len;
        src_va += copy_len;
        remaining -= copy_len;
    }
}

void uvm_copyout(pgtbl_t pgtbl, uint64 dst, uint64 src, uint32 len)
{
    char* src_ptr = (char*)src;
    uint64 dst_va = dst;
    uint32 remaining = len;
    
    while(remaining > 0) {
        uint64 dst_page_base = PG_ROUND_DOWN(dst_va);
        uint64 offset_in_page = dst_va - dst_page_base;
        uint32 copy_len = PGSIZE - offset_in_page;
        if(copy_len > remaining) {
            copy_len = remaining;
        }
        
        pte_t* pte = vm_getpte(pgtbl, dst_va, false);
        assert(pte != NULL && ((*pte) & PTE_V), "uvm_copyout: invalid address");
        
        uint64 pa = PTE_TO_PA(*pte);
        char* dst_ptr = (char*)(pa + offset_in_page);
        
        memmove(dst_ptr, src_ptr, copy_len);
        
        src_ptr += copy_len;
        dst_va += copy_len;
        remaining -= copy_len;
    }
}

void uvm_copyin_str(pgtbl_t pgtbl, uint64 dst, uint64 src, uint32 maxlen)
{
    char* dst_ptr = (char*)dst;
    uint64 src_va = src;
    uint32 copied = 0;
    
    while(copied < maxlen) {
        uint64 src_page_base = PG_ROUND_DOWN(src_va);
        uint64 offset_in_page = src_va - src_page_base;
        
        pte_t* pte = vm_getpte(pgtbl, src_va, false);
        assert(pte != NULL && ((*pte) & PTE_V), "uvm_copyin_str: invalid address");
        
        uint64 pa = PTE_TO_PA(*pte);
        char* src_ptr = (char*)(pa + offset_in_page);
        
        while(offset_in_page < PGSIZE && copied < maxlen) {
            *dst_ptr = *src_ptr;
            if(*src_ptr == '\0') {
                return;
            }
            dst_ptr++;
            src_ptr++;
            src_va++;
            offset_in_page++;
            copied++;
        }
    }
    
    if(copied == maxlen) {
        *(dst_ptr - 1) = '\0';
    }
}