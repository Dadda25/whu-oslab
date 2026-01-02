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
            continue;
        }
        
        pa = (uint64)PTE_TO_PA(*pte);
        flags = (int)PTE_FLAGS(*pte);

        page = (uint64)pmem_alloc(false);
        memmove((char*)page, (const char*)pa, PGSIZE);
        vm_mappages(new, va, page, PGSIZE, flags);
    }
}

// 两个 mmap_region 区域合并
// 保留一个 释放一个 不操作 next 指针
// 在uvm_munmap里使用
/*
static void mmap_merge(mmap_region_t* mmap_1, mmap_region_t* mmap_2, bool keep_mmap_1)
{
    // 确保有效和紧临
    assert(mmap_1 != NULL && mmap_2 != NULL, "mmap_merge: NULL");
    assert(mmap_1->begin + mmap_1->npages * PGSIZE == mmap_2->begin, "mmap_merge: check fail");
    
    // merge
    if(keep_mmap_1) {
        mmap_1->npages += mmap_2->npages;
        mmap_region_free(mmap_2);
    } else {
        mmap_2->begin -= mmap_1->npages * PGSIZE;
        mmap_2->npages += mmap_1->npages;
        mmap_region_free(mmap_1);
    }
}
*/

// 打印以 mmap 为首的 mmap 链
// for debug
void uvm_show_mmaplist(mmap_region_t* mmap)
{
    mmap_region_t* tmp = mmap;
    printf("\nmmap allocable area:\n");
    if(tmp == NULL)
        printf("NULL\n");
    while(tmp != NULL) {
        printf("allocable region: %p ~ %p\n", tmp->begin, tmp->begin + tmp->npages * PGSIZE);
        tmp = tmp->next;
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
                pmem_free((uint64)next_pgtbl, true);
            } else {
                // level == 1，这是最后一级页表，释放物理页
                uint64 pa = PTE_TO_PA(pte);
                pmem_free(pa, false);
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
    pmem_free((uint64)pgtbl, true);
}

// 拷贝页表 (拷贝并不包括trapframe 和 trampoline)
void uvm_copy_pgtbl(pgtbl_t new, pgtbl_t old, uint64 heap_top, uint32 ustack_pages, mmap_region_t* mmap)
{
    /* step-1: USER_BASE ~ heap_top */
    if(heap_top > PGSIZE) {
        copy_range(old, new, PGSIZE, PG_ROUND_UP(heap_top));
    }

    /* step-2: ustack */
    uint64 ustack_top = TRAPFRAME;
    uint64 ustack_bottom = ustack_top - ustack_pages * PGSIZE;
    if(ustack_pages > 0) {
        copy_range(old, new, ustack_bottom, ustack_top);
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
        uint64 pa = (uint64)pmem_alloc(false);
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
        
        // 情况1: 当前区域与要释放的区域没有交集
        if(curr_end <= begin || curr_begin >= end) {
            prev = curr;
            curr = curr->next;
            continue;
        }
        
        // 情况2: 要释放的区域完全包含当前区域
        if(begin <= curr_begin && end >= curr_end) {
            // 删除整个区域
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
        
        // 情况3: 释放区域在当前区域的开头
        if(begin <= curr_begin && end < curr_end) {
            uint32 removed_pages = (end - curr_begin) / PGSIZE;
            curr->begin = end;
            curr->npages -= removed_pages;
            prev = curr;
            curr = curr->next;
            continue;
        }
        
        // 情况4: 释放区域在当前区域的结尾
        if(begin > curr_begin && end >= curr_end) {
            uint32 removed_pages = (curr_end - begin) / PGSIZE;
            curr->npages -= removed_pages;
            prev = curr;
            curr = curr->next;
            continue;
        }
        
        // 情况5: 释放区域在当前区域的中间，需要分裂成两个区域
        if(begin > curr_begin && end < curr_end) {
            // 创建新区域存储后半部分
            mmap_region_t* new_region = mmap_region_alloc();
            new_region->begin = end;
            new_region->npages = (curr_end - end) / PGSIZE;
            new_region->next = curr->next;
            
            // 修改当前区域为前半部分
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

// 用户堆空间增加, 返回新的堆顶地址 (注意栈顶最大值限制)
// 在这里无需修正 p->heap_top
uint64 uvm_heap_grow(pgtbl_t pgtbl, uint64 heap_top, uint32 len)
{
    uint64 new_heap_top = heap_top + len;
    uint64 heap_top_aligned = PG_ROUND_UP(heap_top);
    uint64 new_heap_top_aligned = PG_ROUND_UP(new_heap_top);
    
    // 检查是否超出限制（不能碰到用户栈）
    // 假设用户栈在TRAPFRAME下方，留出足够空间
    if(new_heap_top_aligned >= TRAPFRAME - 256 * PGSIZE) {
        return heap_top; // 增长失败，返回原值
    }
    
    // 如果需要分配新页
    if(new_heap_top_aligned > heap_top_aligned) {
        uint64 npages = (new_heap_top_aligned - heap_top_aligned) / PGSIZE;
        for(uint64 i = 0; i < npages; i++) {
            uint64 va = heap_top_aligned + i * PGSIZE;
            uint64 pa = (uint64)pmem_alloc(false);
            vm_mappages(pgtbl, va, pa, PGSIZE, PTE_R | PTE_W | PTE_U);
        }
    }

    return new_heap_top;
}

// 用户堆空间减少, 返回新的堆顶地址
// 在这里无需修正 p->heap_top
uint64 uvm_heap_ungrow(pgtbl_t pgtbl, uint64 heap_top, uint32 len)
{
    if(len >= heap_top) {
        return 0; // 不能减少到负数
    }
    
    uint64 new_heap_top = heap_top - len;
    uint64 heap_top_aligned = PG_ROUND_UP(heap_top);
    uint64 new_heap_top_aligned = PG_ROUND_UP(new_heap_top);
    
    // 如果需要释放页
    if(new_heap_top_aligned < heap_top_aligned) {
        uint64 npages = (heap_top_aligned - new_heap_top_aligned) / PGSIZE;
        vm_unmappages(pgtbl, new_heap_top_aligned, npages * PGSIZE, true);
    }

    return new_heap_top;
}

// 用户态地址空间[src, src+len) 拷贝至 内核态地址空间[dst, dst+len)
// 注意: src dst 不一定是 page-aligned
void uvm_copyin(pgtbl_t pgtbl, uint64 dst, uint64 src, uint32 len)
{
    char* dst_ptr = (char*)dst;
    uint64 src_va = src;
    uint32 remaining = len;
    
    while(remaining > 0) {
        // 获取源地址所在页的PTE
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

// 内核态地址空间[src, src+len） 拷贝至 用户态地址空间[dst, dst+len)
void uvm_copyout(pgtbl_t pgtbl, uint64 dst, uint64 src, uint32 len)
{
    char* src_ptr = (char*)src;
    uint64 dst_va = dst;
    uint32 remaining = len;
    
    while(remaining > 0) {
        // 获取目标地址所在页的PTE
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

// 用户态字符串拷贝到内核态
// 最多拷贝maxlen字节, 中途遇到'\0'则终止
// 注意: src dst 不一定是 page-aligned
void uvm_copyin_str(pgtbl_t pgtbl, uint64 dst, uint64 src, uint32 maxlen)
{
    char* dst_ptr = (char*)dst;
    uint64 src_va = src;
    uint32 copied = 0;
    
    while(copied < maxlen) {
        // 获取源地址所在页的PTE
        uint64 src_page_base = PG_ROUND_DOWN(src_va);
        uint64 offset_in_page = src_va - src_page_base;
        
        pte_t* pte = vm_getpte(pgtbl, src_va, false);
        assert(pte != NULL && ((*pte) & PTE_V), "uvm_copyin_str: invalid address");
        
        uint64 pa = PTE_TO_PA(*pte);
        char* src_ptr = (char*)(pa + offset_in_page);
        
        // 逐字节拷贝直到遇到'\0'或页边界或达到maxlen
        while(offset_in_page < PGSIZE && copied < maxlen) {
            *dst_ptr = *src_ptr;
            if(*src_ptr == '\0') {
                return; // 遇到结束符，完成拷贝
            }
            dst_ptr++;
            src_ptr++;
            src_va++;
            offset_in_page++;
            copied++;
        }
    }
    
    // 如果达到maxlen还没遇到'\0'，手动添加结束符
    if(copied == maxlen) {
        *(dst_ptr - 1) = '\0';
    }
}