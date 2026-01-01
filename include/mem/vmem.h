#ifndef __KVMEM_H__
#define __KVMEM_H__

#include "common.h"
#include "mmap.h"

/*
    我们使用RISC-V体系结构中的SV39作为虚拟内存的设计规范

    satp寄存器: MODE(4) + ASID(16) + PPN(44)
    MODE控制虚拟内存模式 ASID与Flash刷新有关 PPN存放页表基地址

    基础页面 4KB
    
    VA和PA的构成:
    VA: VPN[2] + VPN[1] + VPN[0] + offset    9 + 9 + 9 + 12 = 39 (使用uint64存储) => 最大虚拟地址为512GB 
    PA: PPN[2] + PPN[1] + PPN[0] + offset   26 + 9 + 9 + 12 = 56 (使用uint64存储)
    
    为什么是 "9" : 4KB / uint64 = 512 = 2^9 所以一个物理页可以存放512个页表项
    我们使用三级页表对应三级VPN, VPN[2]称为顶级页表、VPN[1]称为次级页表、VPN[0]称为低级页表

    PTE定义:
    reserved + PPN[2] + PPN[1] + PPN[0] + RSW + D A G U X W R V  共64bit
       10        26       9        9       2    1 1 1 1 1 1 1 1
    
    需要关注的部分:
    V : valid
    X W R : execute write read (全0意味着这是页表所在的物理页)
    U : 用户态是否可以访问
    PPN区域 : 存放物理页号

*/

// 页表项
typedef uint64 pte_t;

// 顶级页表
typedef uint64* pgtbl_t;

// satp寄存器相关
#define SATP_SV39 (8L << 60)  // MODE = SV39
#define MAKE_SATP(pagetable) (SATP_SV39 | (((uint64)pagetable) >> 12)) // 设置MODE和PPN字段

/*---------------------- in kvm.c -------------------------*/

void   vm_print(pgtbl_t pgtbl);
pte_t* vm_getpte(pgtbl_t pgtbl, uint64 va, bool alloc);
uint64 vm_getpa(pgtbl_t pgtbl, uint64 va);
void   vm_mappages(pgtbl_t pgtbl, uint64 va, uint64 pa, uint64 len, int perm);
void   vm_unmappages(pgtbl_t pgtbl, uint64 va, uint64 len, bool freeit);

pgtbl_t kvm_create(void);
void    kvm_init();
void    kvm_inithart();

/*------------------------ in uvm.c -----------------------*/

void   uvm_show_mmaplist(mmap_region_t* mmap);

void   uvm_destroy_pgtbl(pgtbl_t pgtbl);
void   uvm_copy_pgtbl(pgtbl_t old, pgtbl_t new, uint64 heap_top, uint32 ustack_pages, mmap_region_t* mmap);

void   uvm_mmap(uint64 begin, uint32 npages, int perm);
void   uvm_munmap(uint64 begin, uint32 npages);

uint64 uvm_heap_grow(pgtbl_t pgtbl, uint64 heap_top, uint32 len);
uint64 uvm_heap_ungrow(pgtbl_t pgtbl, uint64 heap_top, uint32 len);

void   uvm_copyin(pgtbl_t pgtbl, uint64 dst, uint64 src, uint32 len);
void   uvm_copyout(pgtbl_t pgtbl, uint64 dst, uint64 src, uint32 len);
void   uvm_copyin_str(pgtbl_t pgtbl, uint64 dst, uint64 src, uint32 maxlen);

#endif