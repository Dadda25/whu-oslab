#ifndef __LOCK_H__
#define __LOCK_H__

#include "common.h"

typedef struct spinlock {
    int locked;         // 该锁是否已被所住
    char* name;         // 锁的姓名
    int cpuid;          // 持有该锁的核心编号
} spinlock_t;

typedef struct sleeplock {
  int locked;           // 该锁是否已被锁住
  struct spinlock lk;   // 维护该睡眠锁信息的自旋锁
  char *name;           // 锁的姓名（同自旋锁）
  int pid;              // 持有该锁的进程号
} sleeplock_t;

// 简单的开关中断函数
void push_off();
void pop_off();

// 自旋锁相关函数
void spinlock_init(spinlock_t* lk, char* name);
void spinlock_acquire(spinlock_t* lk);
void spinlock_release(spinlock_t* lk);
bool spinlock_holding(spinlock_t* lk);

// 睡眠锁相关函数
void sleeplock_init(sleeplock_t* slk, char *name);
void sleeplock_acquire(sleeplock_t* slk);
void sleeplock_release(sleeplock_t* slk);
bool sleeplock_holding(sleeplock_t* slk);

#endif