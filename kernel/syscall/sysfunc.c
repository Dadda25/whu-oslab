#include "proc/cpu.h"
#include "proc/proc.h"
#include "mem/vmem.h"
#include "mem/pmem.h"
#include "mem/mmap.h"
#include "lib/str.h"
#include "lib/print.h"
#include "dev/timer.h"
#include "syscall/sysfunc.h"
#include "syscall/syscall.h"
#include "syscall/sysnum.h"
#include "memlayout.h"
#include "riscv.h"
#include "common.h"


// 堆伸缩
// uint64 new_heap_top 新的堆顶 (如果是0代表查询, 返回旧的堆顶)
// 成功返回新的堆顶 失败返回-1
uint64 sys_brk(void) {
    uint64 arg0;
    arg_uint64(0, &arg0);
    
    proc_t* p = myproc();
    
    // 如果 arg0 为 0，返回当前堆顶
    if(arg0 == 0) {
        return p->heap_top;
    }
    
    uint64 old_heap_top = p->heap_top;
    uint64 new_heap_top = PG_ROUND_UP(arg0);
    
    // 检查是否超过最大堆大小
    uint64 max_heap = TRAPFRAME - p->ustack_pages * PGSIZE - PGSIZE;
    if(new_heap_top > max_heap) {
        return -1;
    }
    
    if(new_heap_top > old_heap_top) {
        // 增长堆
        uint32 len = new_heap_top - old_heap_top;
        if(uvm_heap_grow(p->pgtbl, old_heap_top, len) < 0) {
            return -1;
        }
    } else if(new_heap_top < old_heap_top) {
        // 缩减堆
        uint32 len = old_heap_top - new_heap_top;
        if(uvm_heap_ungrow(p->pgtbl, new_heap_top, len) < 0) {
            return -1;
        }
    }
    
    p->heap_top = new_heap_top;
    return new_heap_top;
}

// 内存映射
// uint64 start 起始地址 (如果为0则由内核自主选择一个合适的起点)
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
        return -1;
    }
    
    uint32 npages = len / PGSIZE;
    
    // 如果 start == 0，自动选择一个合适的地址
    if(start == 0) {
        uint64 search_start = PG_ROUND_UP(p->heap_top) + PGSIZE;
        uint64 search_end = TRAPFRAME - p->ustack_pages * PGSIZE - PGSIZE;
        
        mmap_region_t* curr = p->mmap;
        uint64 candidate = search_start;
        
        while(curr != NULL) {
            uint64 curr_end = curr->begin + curr->npages * PGSIZE;
            
            if(candidate + len <= curr->begin) {
                start = candidate;
                break;
            }
            
            candidate = curr_end;
            curr = curr->next;
        }
        
        if(start == 0) {
            if(candidate + len <= search_end) {
                start = candidate;
            } else {
                return -1;
            }
        }
    } else {
        if(start % PGSIZE != 0) {
            return -1;
        }
    }
    
    // 执行mmap映射
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

// 打印字符串
// uint64 addr
uint64 sys_print()
{
    char buf[256];
    
    arg_str(0, buf, sizeof(buf));
    
    printf("%s", buf);
    
    return 0;
}

// 进程复制
uint64 sys_fork()
{
    return proc_fork();
}

// 进程等待
// uint64 addr  子进程退出时的exit_state需要放到这里 
uint64 sys_wait()
{
    uint64 addr;
    arg_uint64(0, &addr);
    
    return proc_wait(addr);
}

// 进程退出
// int exit_state
uint64 sys_exit()
{
    int exit_state;
    arg_uint32(0, (uint32*)&exit_state);
    
    proc_exit(exit_state);
    
    return 0;  // 永远不会执行到这里
}

extern timer_t sys_timer;

// 进程睡眠一段时间
// uint32 second 睡眠时间
// 成功返回0, 失败返回-1
uint64 sys_sleep(void) {
    uint32 second;
    arg_uint32(0, &second);
    
    // 获取当前 tick
    uint64 start_tick = timer_get_ticks();
    // 计算目标 tick（秒数 * 频率）
    uint64 target_tick = start_tick + second * TIMER_FREQ;
    
    // 等待直到达到目标时间
    while(timer_get_ticks() < target_tick) {
        // 检查进程是否被杀死
        if(myproc()->state == ZOMBIE) {
            return -1;
        }
        // 主动让出 CPU
        proc_yield();
    }
    
    return 0;
}