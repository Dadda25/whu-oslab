#include "proc/cpu.h"
#include "mem/vmem.h"
#include "mem/pmem.h"
#include "mem/mmap.h"
#include "lib/str.h"
#include "lib/print.h"
#include "syscall/sysfunc.h"
#include "syscall/syscall.h"
#include "memlayout.h"
#include "common.h"
#include "riscv.h"

// ============================================================================
// 如果proc_t中没有mmap字段，使用与uvm.c相同的全局数组管理
// ============================================================================
#define MAX_PROCS 64

// ============================================================================
// 系统调用实现
// ============================================================================

// 堆伸缩
// uint64 new_heap_top 新的堆顶 (如果是0代表查询, 返回旧的堆顶)
// 成功返回新的堆顶 失败返回-1
uint64 sys_brk()
{
    proc_t* p = myproc();
    uint64 new_heap_top;
    arg_uint64(0, &new_heap_top);
    
    // 如果 new_heap_top == 0，查询当前堆顶
    if(new_heap_top == 0) {
        return p->heap_top;
    }
    
    uint64 old_heap_top = p->heap_top;
    
    // 检查新堆顶是否合法
    if(new_heap_top < old_heap_top) {
        // 堆收缩
        uint32 shrink_len = old_heap_top - new_heap_top;
        uint64 result = uvm_heap_ungrow(p->pgtbl, old_heap_top, shrink_len);
        if(result == new_heap_top) {
            p->heap_top = new_heap_top;
            return new_heap_top;
        }
        return -1;
    } else if(new_heap_top > old_heap_top) {
        // 堆增长
        uint32 grow_len = new_heap_top - old_heap_top;
        uint64 result = uvm_heap_grow(p->pgtbl, old_heap_top, grow_len);
        if(result == new_heap_top) {
            p->heap_top = new_heap_top;
            return new_heap_top;
        }
        return -1;
    }
    
    // new_heap_top == old_heap_top，不需要修改
    return old_heap_top;
}

// 内存映射
// uint64 start 起始地址 (如果为0则由内核自主选择一个合适的起点, 通常是顺序扫描找到一个够大的空闲空间)
// uint32 len   范围(字节, 检查是否是page-aligned)
// 成功返回映射空间的起始地址, 失败返回-1
uint64 sys_mmap()
{
    proc_t* p = myproc();
    uint64 start;
    uint32 len;
    
    arg_uint64(0, &start);
    arg_uint32(1, &len);
    
    // 检查长度是否page-aligned
    if(len == 0 || len % PGSIZE != 0) {
        printf("sys_mmap: len not page-aligned or zero\n");
        return -1;
    }
    
    uint32 npages = len / PGSIZE;
    
    // 如果 start == 0，自动选择一个合适的地址
    if(start == 0) {
        // 从堆顶上方开始查找空闲区域
        uint64 search_start = PG_ROUND_UP(p->heap_top) + PGSIZE; // 在堆顶上方留一些空间
        uint64 search_end = TRAPFRAME - p->ustack_pages * PGSIZE - PGSIZE; // 栈下方留一些空间
        
        mmap_region_t* curr = p->mmap;
        uint64 candidate = search_start;
        
        // 遍历已有的mmap区域，找到足够大的空隙
        while(curr != NULL) {
            uint64 curr_end = curr->begin + curr->npages * PGSIZE;
            
            // 检查candidate到curr->begin之间是否有足够空间
            if(candidate + len <= curr->begin) {
                // 找到了合适的空间
                start = candidate;
                break;
            }
            
            // 移动到下一个可能的位置
            candidate = curr_end;
            curr = curr->next;
        }
        
        // 如果遍历完mmap链表还没找到，使用最后一个candidate
        if(start == 0) {
            if(candidate + len <= search_end) {
                start = candidate;
            } else {
                printf("sys_mmap: no space available\n");
                return -1;
            }
        }
    } else {
        // 用户指定了起始地址，检查是否page-aligned
        if(start % PGSIZE != 0) {
            printf("sys_mmap: start not page-aligned\n");
            return -1;
        }
    }
    
    // 执行mmap映射
    // 默认权限：可读可写，用户态可访问
    int perm = PTE_R | PTE_W | PTE_U;
    uvm_mmap(start, npages, perm);
    
    return start;
}

// 取消内存映射
// uint64 start 起始地址
// uint32 len   范围(字节, 检查是否是page-aligned)
// 成功返回0 失败返回-1
uint64 sys_munmap()
{
    uint64 start;
    uint32 len;
    
    arg_uint64(0, &start);
    arg_uint32(1, &len);
    
    // 检查地址和长度是否page-aligned
    if(start % PGSIZE != 0 || len % PGSIZE != 0) {
        printf("sys_munmap: start or len not page-aligned\n");
        return -1;
    }
    
    if(len == 0) {
        printf("sys_munmap: len is zero\n");
        return -1;
    }
    
    uint32 npages = len / PGSIZE;
    
    // 执行munmap
    uvm_munmap(start, npages);
    
    return 0;
}

// copyin 测试 (int 数组)
// uint64 addr
// uint32 len
// 返回 0
uint64 sys_copyin()
{
    proc_t* p = myproc();
    uint64 addr;
    uint32 len;
    arg_uint64(0, &addr);
    arg_uint32(1, &len);
    int tmp;
    for(int i = 0; i < len; i++) {
        uvm_copyin(p->pgtbl, (uint64)&tmp, addr + i * sizeof(int), sizeof(int));
        printf("get a number from user: %d\n", tmp);
    }
    return 0;
}

// copyout 测试 (int 数组)
// uint64 addr
// 返回数组元素数量
uint64 sys_copyout()
{
    int L[5] = {1, 2, 3, 4, 5};
    proc_t* p = myproc();
    uint64 addr;
    arg_uint64(0, &addr);
    uvm_copyout(p->pgtbl, addr, (uint64)L, sizeof(int) * 5);
    return 5;
}

// copyinstr测试
// uint64 addr
// 成功返回0
uint64 sys_copyinstr()
{
    char s[64];
    arg_str(0, s, 64);
    printf("get str from user: %s\n", s);
    return 0;
}

uint64 sys_test()
{
    printf("sys_test: test syscall called\n");
    return 0;
}