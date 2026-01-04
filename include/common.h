// 这个头文件通常认为其他.h文件都应该include
#ifndef __COMMON_H__
#define __COMMON_H__

// 类型定义

typedef char                   int8;
typedef short                  int16;
typedef int                    int32;
typedef long long              int64;
typedef unsigned char          uint8; 
typedef unsigned short         uint16;
typedef unsigned int           uint32;
typedef unsigned long long     uint64;

typedef unsigned long long         reg; 
typedef enum {false = 0, true = 1} bool;

typedef void* addr_t;

#ifndef NULL
#define NULL ((void*)0)
#endif

#define NCPU 2
#define NPROC 64
#define NOFILE       16  // open files per process
#define NFILE       100  // open files per system
#define NINODE      100  // maximum number of active i-nodes

#endif