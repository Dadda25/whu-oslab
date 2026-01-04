#ifndef _FILE_H_
#define _FILE_H_

#include "common.h"
#include "fs.h"

enum EnumFileType {
    FILE_NONE,
    FILE_PIPE,
    FILE_INODE,
    FILE_DEVICE
};

struct File {
    enum EnumFileType type;
    int ref;         // 引用计数
    char readable;
    char writable;
    char append;     // 追加模式
    struct inode *ip;  // 文件对应的 inode
    uint64 off;          // 文件偏移量
    short major;      // 设备号（设备文件）
};

// 打开文件表
#define NFILE 100

struct ftable_s {
    struct File file[NFILE];
};

extern struct ftable_s ftable;

// 文件操作函数
void file_init(void);
struct File* alloc_file(void);
struct File* file_dup(struct File *f);
void file_close(struct File *f);
int file_stat(struct File *f, uint64 addr);
int file_read(struct File *f, uint64 addr, int n);
int file_write(struct File *f, uint64 addr, int n);

#endif // !_FILE_H_