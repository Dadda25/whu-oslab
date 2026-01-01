#include "mem/pmem.h"
#include "lib/print.h"
#include "lib/lock.h"
#include "lib/str.h"
#include "common.h"
#include "riscv.h"

// 物理页节点
typedef struct page_node {
    struct page_node* next;
} page_node_t;

// 许多物理页构成一个可分配的区域
typedef struct alloc_region {
    uint64 begin;          // 起始物理地址
    uint64 end;            // 终止物理地址
    spinlock_t lk;         // 自旋锁(保护下面两个变量)
    uint32 allocable;      // 可分配页面数    
    page_node_t list_head; // 可分配链的链头节点
} alloc_region_t;

// 内核和用户可分配的物理页分开
static alloc_region_t kern_region, user_region;

#define KERN_PAGES 1024 // 内核可分配空间占1024个pages

// 物理内存初始化
void pmem_init(void)
{
    // 初始化内核区域
    kern_region.begin = (uint64)ALLOC_BEGIN;
    kern_region.end = (uint64)ALLOC_BEGIN + KERN_PAGES * PGSIZE;
    spinlock_init(&kern_region.lk, "kern_region");
    kern_region.allocable = KERN_PAGES;
    kern_region.list_head.next = NULL;
    
    // 初始化用户区域
    user_region.begin = kern_region.end;
    user_region.end = (uint64)ALLOC_END;
    spinlock_init(&user_region.lk, "user_region");
    user_region.allocable = (user_region.end - user_region.begin) / PGSIZE;
    user_region.list_head.next = NULL;
    
    // 将内核区域的所有物理页加入链表
    page_node_t* p = &kern_region.list_head;
    for(uint64 pa = kern_region.begin; pa < kern_region.end; pa += PGSIZE) {
        p->next = (page_node_t*)pa;
        p = p->next;
    }
    p->next = NULL;
    
    // 将用户区域的所有物理页加入链表
    p = &user_region.list_head;
    for(uint64 pa = user_region.begin; pa < user_region.end; pa += PGSIZE) {
        p->next = (page_node_t*)pa;
        p = p->next;
    }
    p->next = NULL;
    
    printf("pmem_init: kernel region [%p, %p) pages=%d\n", 
           kern_region.begin, kern_region.end, kern_region.allocable);
    printf("pmem_init: user region [%p, %p) pages=%d\n", 
           user_region.begin, user_region.end, user_region.allocable);
}

// 返回一个可分配的干净物理页
// 失败则panic锁死
void* pmem_alloc(bool in_kernel)
{
    alloc_region_t* region = in_kernel ? &kern_region : &user_region;
    
    spinlock_acquire(&region->lk);
    
    // 从链表头取出一个页面
    page_node_t* page = region->list_head.next;
    if(page == NULL) {
        spinlock_release(&region->lk);
        panic("pmem_alloc: out of memory");
    }
    
    region->list_head.next = page->next;
    region->allocable--;
    
    spinlock_release(&region->lk);
    
    // 清空页面内容
    memset((void*)page, 0, PGSIZE);
    
    return (void*)page;
}

// 释放物理页
// 失败则panic锁死
void pmem_free(uint64 page, bool in_kernel)
{
    // 检查地址对齐
    if(page % PGSIZE != 0) {
        panic("pmem_free: page not aligned");
    }
    
    alloc_region_t* region = in_kernel ? &kern_region : &user_region;
    
    // 检查地址范围
    if(page < region->begin || page >= region->end) {
        panic("pmem_free: page out of range");
    }
    
    spinlock_acquire(&region->lk);
    
    // 将页面插入链表头
    page_node_t* p = (page_node_t*)page;
    p->next = region->list_head.next;
    region->list_head.next = p;
    region->allocable++;
    
    spinlock_release(&region->lk);
}