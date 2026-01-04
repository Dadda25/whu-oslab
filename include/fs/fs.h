#ifndef _FS_H_
#define _FS_H_

#include "common.h"

// 文件系统基本参数
#define BSIZE 1024                  // 块大小
#define FSSIZE 1000                 // 文件系统块数
#define LOGSIZE 30                  // 日志块数
#define ROOTDEV 1                   // 根设备号
#define ROOTINO 1                   // 根目录 inode 号
#define FSMAGIC 0x10203040         // 文件系统魔数

// inode 块地址数组参数
#define NDIRECT 12                  // 直接块数量
#define NINDIRECT (BSIZE / sizeof(uint32))  // 间接块数量
#define MAXFILE (NDIRECT + NINDIRECT)    // 最大文件块数

// 文件系统布局：
// [ boot block | super block | log | inode blocks | free bit map | data blocks ]

// 磁盘上的超级块结构
struct superblock {
    uint64 magic;        // 魔数，用于验证文件系统
    uint64 size;         // 文件系统大小（块数）
    uint64 nblocks;      // 数据块数量
    uint64 ninodes;      // inode 数量
    uint64 nlog;         // 日志块数量
    uint64 logstart;     // 日志起始块号
    uint64 inodestart;   // inode 区起始块号
    uint64 bmapstart;    // 位图起始块号
};

// 磁盘上的 inode 结构
struct dinode {
    short type;               // 文件类型
    short major;              // 主设备号（设备文件）
    short minor;              // 次设备号（设备文件）
    short nlink;              // 硬链接数
    uint32 size;                 // 文件大小（字节）
    uint32 addrs[NDIRECT + 1];   // 数据块地址数组
};

// 内存中的 inode 结构
struct inode {
    uint64 dev;           // 设备号
    uint64 inum;          // inode 号
    int ref;           // 引用计数
    int valid;         // inode 是否已从磁盘加载

    short type;        // 文件类型
    short major;       // 主设备号
    short minor;       // 次设备号
    short nlink;       // 硬链接数
    uint32 size;          // 文件大小（字节）
    uint32 addrs[NDIRECT + 1];  // 数据块地址数组
};

// 目录项结构
#define DIRSIZ 14

struct dirent {
    uint64 inum;          // inode 号
    char name[DIRSIZ]; // 文件名
};

// 每个块的 inode 数量
#define IPB (BSIZE / sizeof(struct dinode))

// 计算包含 inode i 的块号
#define IBLOCK(i, sb) ((i) / IPB + sb.inodestart)

// 每个块的位图位数
#define BPB (BSIZE * 8)

// 计算包含块 b 的位图块号
#define BBLOCK(b, sb) ((b) / BPB + sb.bmapstart)

// 文件类型
#define T_DIR   1   // 目录
#define T_FILE  2   // 普通文件
#define T_DEVICE 3  // 设备

// 文件stat结构
struct stat {
    int dev;     // 文件系统的磁盘设备
    uint64 ino;     // Inode号
    short type;  // 文件类型
    short nlink; // 指向文件的链接数
    uint64 size;    // 文件字节数
};

// 文件打开标志
#define O_RDONLY  0x000
#define O_WRONLY  0x001
#define O_RDWR    0x002
#define O_CREATE  0x200
#define O_TRUNC   0x400
#define O_APPEND  0x800

// 文件系统函数声明
void fs_init(int dev);
int fs_alloc(uint64 dev);
void fs_free(int dev, uint64 b);
struct inode* ialloc(uint64 dev, short type);
struct inode* iget(uint64 dev, uint64 inum);
struct inode* idup(struct inode *ip);
void ilock(struct inode *ip);
void iunlock(struct inode *ip);
void iput(struct inode *ip);
void iunlockput(struct inode *ip);
void iupdate(struct inode *ip);
int readi(struct inode *ip, int user_dst, uint64 dst, uint64 off, uint64 n);
int writei(struct inode *ip, int user_src, uint64 src, uint64 off, uint64 n);
void itrunc(struct inode *ip);
struct inode* dirlookup(struct inode *dp, char *name, uint64 *poff);
int dirlink(struct inode *dp, char *name, uint64 inum);
struct inode* namei(char *path);
struct inode* nameiparent(char *path, char *name);
void stati(struct inode *ip, struct stat *st);

#endif // _FS_H_
