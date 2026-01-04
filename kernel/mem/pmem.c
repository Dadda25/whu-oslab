#include "mem/pmem.h"
#include "lib/lock.h"
#include "lib/str.h"
#include "lib/print.h"
#include "common.h"
#include "riscv.h"

// 物理页节点
typedef struct page_node{
    struct page_node* next;
} page_node_t;

// 许多物理页构成一个可分配的区域
typedef struct alloc_region { 
    uint64 begin;           // 起始物理地址
    uint64 end;             // 终止物理地址
    spinlock_t lk;          // 自旋锁(保护下面两个变量) 
    uint32 allocable;       // 可分配页面数
    page_node_t list_head;  // 可分配链的链头节点
}alloc_region_t;            // 内核和用户可分配的物理页分开
 
static alloc_region_t kern_region, user_region;

// 定义分配到内核的物理页数（多一些）
#define KERNEL_PAGES 2048

// 分别对kernel和user下的可分配区域进行初始化操作
void  pmem_init(void){
    kern_region.begin = (uint64)&ALLOC_BEGIN;
    kern_region.end = (uint64)&ALLOC_BEGIN + KERNEL_PAGES * PGSIZE;
    kern_region.allocable = 0;
    kern_region.list_head.next = NULL;
    spinlock_init(&kern_region.lk, "kern_pmem_lock");
    for (uint64 addr = kern_region.begin; addr < kern_region.end; addr += PGSIZE) {
        // 将物理页首地址转换为 page_node_t 指针
        page_node_t *page = (page_node_t *)addr;
        // 使用头插法插入链表
        page->next = kern_region.list_head.next;
        kern_region.list_head.next = page;
        kern_region.allocable++; // 增加可分配页数
    }

    user_region.begin = (uint64)&ALLOC_BEGIN + KERNEL_PAGES * PGSIZE;
    user_region.end = (uint64)&ALLOC_END;
    user_region.allocable = 0;
    user_region.list_head.next = NULL;
    spinlock_init(&user_region.lk, "user_pmem_lock");
    for (uint64 addr = user_region.begin; addr < user_region.end; addr += PGSIZE) {
        // 将物理页首地址转换为 page_node_t 指针
        page_node_t *page = (page_node_t *)addr;
        // 使用头插法插入链表
        page->next = user_region.list_head.next;
        user_region.list_head.next = page;
        user_region.allocable++; // 增加可分配页数
    }
}

void* pmem_alloc(bool in_kernel){
    alloc_region_t *region = in_kernel ? &kern_region : &user_region;
    spinlock_acquire(&region->lk); // 上锁

    if(region->allocable == 0){
        spinlock_release(&region->lk);
        panic("alloc pages not enough");
        return NULL; // 无可用页
    }

    page_node_t *page = region->list_head.next;
    if(page)
        region->list_head.next = page->next;
    region->allocable--;

    spinlock_release(&region->lk); // 解锁

    // 将垃圾填入，防止额外定义
    memset(page, 5, PGSIZE);

    return (void*)page;
}

// 这个page是物理地址
void  pmem_free(uint64 page, bool in_kernel){

    if((page % PGSIZE) != 0 || (char*)page < ALLOC_BEGIN || (char *)page >= ALLOC_END)
        panic("kfree");

    alloc_region_t *region = in_kernel ? &kern_region : &user_region;
    page_node_t *page_ptr = (page_node_t *)page;

    // 将垃圾填入，防止额外定义
    memset((char*)page, 1, PGSIZE);
    
    spinlock_acquire(&region->lk); // 上锁

    page_ptr->next = kern_region.list_head.next;
    kern_region.list_head.next = page_ptr;
    region->allocable++;
    
    spinlock_release(&region->lk); // 解锁
}

// 不用判断物理地址的物理页释放
// 自动由物理地址判断属于哪个链表
void pmem_free_auto(uint64 page){
    bool atKernel = kern_region.begin <= page && page < kern_region.end;
    bool atUser = user_region.begin <= page && page < user_region.end;
    if(atKernel){
        pmem_free(page, 1);
    }
    else if(atUser){
        pmem_free(page, 0);
    }
    else{
        panic("pmem_free: Don't know where pysical page is.");
    }
}