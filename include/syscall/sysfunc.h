#ifndef __SYSFUNC_H__
#define __SYSFUNC_H__

#include "common.h"

uint64 sys_test();
uint64 sys_print();
uint64 sys_brk();
uint64 sys_open();
uint64 sys_close();
uint64 sys_fork();
uint64 sys_wait();
uint64 sys_exit();
uint64 sys_sleep();
uint64 sys_kill();
uint64 sys_getpid();
uint64 sys_read();
uint64 sys_write();
uint64 sys_mkdir();
uint64 sys_link();
uint64 sys_unlink();
uint64 sys_fstat();
uint64 sys_dup();
uint64 sys_yield();
uint64 sys_getticks();

#endif