#include "proc/cpu.h"
#include "mem/vmem.h"
#include "mem/pmem.h"
#include "lib/str.h"
#include "fs/fs.h"
#include "fs/file.h"
#include "fs/log.h"
#include "lib/print.h"
#include "syscall/sysfunc.h"
#include "syscall/syscall.h"
#include "riscv.h"

// 辅助函数：检查目录是否为空
static int isdirempty(struct inode *dp) {
    int off;
    struct dirent de;
    
    for(off = 2 * sizeof(de); off < dp->size; off += sizeof(de)) {
        if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
            panic("isdirempty: readi");
        if(de.inum != 0)
            return 0;
    }
    return 1;
}

// 从进程的文件描述符表中分配一个文件描述符
static int fdalloc(struct File *f) {
    int fd;
    proc_t *p = myproc();
    
    for(fd = 0; fd < NOFILE; fd++) {
        if(p->ofile[fd] == 0) {
            p->ofile[fd] = f;
            return fd;
        }
    }
    return -1;
}

// 创建 inode
struct inode* create(char *path, short type, short major, short minor) {
    struct inode *ip, *dp;
    char name[DIRSIZ];
    
    if((dp = nameiparent(path, name)) == 0)
        return 0;
    
    ilock(dp);
    
    if((ip = dirlookup(dp, name, 0)) != 0) {
        iunlockput(dp);
        ilock(ip);
        if(type == T_FILE && (ip->type == T_FILE || ip->type == T_DEVICE))
            return ip;
        iunlockput(ip);
        return 0;
    }
    
    if((ip = ialloc(dp->dev, type)) == 0)
        panic("create: ialloc");
    
    ilock(ip);
    ip->major = major;
    ip->minor = minor;
    ip->nlink = 1;
    iupdate(ip);
    
    if(type == T_DIR) {
        dp->nlink++;
        iupdate(dp);
        // 创建 . 和 ..
        if(dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0)
            panic("create: dots");
    }
    
    if(dirlink(dp, name, ip->inum) < 0)
        panic("create: dirlink");
    
    iunlockput(dp);
    
    return ip;
}

// sys_open - 打开文件
uint64 sys_open(void) {
    char path[128];
    int fd, omode;
    struct File *f;
    struct inode *ip;
    
    arg_int(1, &omode);
    
    // 从用户空间复制路径
    uint64 path_ptr;
    arg_uint64(0, &path_ptr);
    if(uvm_copyin(myproc()->pgtbl, (uint64)path, path_ptr, sizeof(path)) < 0){
        return -1;
    }
    path[127] = 0;
    
    begin_op();
    
    if(omode & O_CREATE) {
        ip = create(path, T_FILE, 0, 0);
        if(ip == 0) {
            end_op();
            return -1;
        }
    } else {
        if((ip = namei(path)) == 0) {
            end_op();
            return -1;
        }
        ilock(ip);
        if(ip->type == T_DIR && omode != O_RDONLY) {
            iunlockput(ip);
            end_op();
            return -1;
        }
    }
    
    if((f = alloc_file()) == 0) {
        iunlockput(ip);
        end_op();
        return -1;
    }
    
    if((fd = fdalloc(f)) < 0) {
        file_close(f);
        iunlockput(ip);
        end_op();
        return -1;
    }
    
    if(ip->type == T_DEVICE) {
        f->type = FILE_DEVICE;
        f->major = ip->major;
    } else {
        f->type = FILE_INODE;
        f->off = 0;
    }
    f->ip = ip;
    f->readable = !(omode & O_WRONLY);
    f->writable = (omode & O_WRONLY) || (omode & O_RDWR);
    f->append = !!(omode & O_APPEND);  // 转换为布尔值
    
    iunlock(ip);
    end_op();
    
    return fd;
}

// sys_close - 关闭文件
uint64 sys_close(void) {
    int fd;
    struct File *f;
    
    if(arg_fd(0, &fd, &f) < 0)
        return -1;
    myproc()->ofile[fd] = 0;
    file_close(f);
    return 0;
}

// sys_read - 读取文件
uint64 sys_read(void) {
    struct File *f;
    int n;
    uint64 p;
    
    if(arg_fd(0, 0, &f) < 0)
        return -1;
    arg_int(2, &n);
    arg_uint64(1, &p);
    
    return file_read(f, p, n);
}

// sys_write - 写入文件
uint64 sys_write(void) {
    struct File *f;
    int n;
    uint64 p;
    
    if(arg_fd(0, 0, &f) < 0){
        return -1;        
    }
    arg_int(2, &n);
    arg_uint64(1, &p);
    
    return file_write(f, p, n);
}

// sys_mkdir - 创建目录
uint64 sys_mkdir(void) {
    char path[128];
    struct inode *ip;
    
    uint64 path_ptr;
    arg_uint64(0, &path_ptr);
    if(uvm_copyin(myproc()->pgtbl, (uint64)path, path_ptr, sizeof(path)) < 0)
        return -1;
    path[127] = 0;
    
    begin_op();
    if((ip = create(path, T_DIR, 0, 0)) == 0) {
        end_op();
        return -1;
    }
    iunlockput(ip);
    end_op();
    
    return 0;
}

// sys_unlink - 删除文件
uint64 sys_unlink(void) {
    char path[128];
    struct inode *ip, *dp;
    struct dirent de;
    char name[DIRSIZ];
    uint64 off;
    
    uint64 path_ptr;
    arg_uint64(0, &path_ptr);
    if(uvm_copyin(myproc()->pgtbl, (uint64)path, path_ptr, sizeof(path)) < 0)
        return -1;
    path[127] = 0;
    
    begin_op();
    
    if((dp = nameiparent(path, name)) == 0) {
        end_op();
        return -1;
    }
    
    ilock(dp);
    
    // 不能删除 . 和 ..
    if(strncmp(name, ".", DIRSIZ) == 0 || strncmp(name, "..", DIRSIZ) == 0) {
        iunlockput(dp);
        end_op();
        return -1;
    }
    
    if((ip = dirlookup(dp, name, &off)) == 0) {
        iunlockput(dp);
        end_op();
        return -1;
    }
    ilock(ip);
    
    if(ip->nlink < 1)
        panic("unlink: nlink < 1");
    
    if(ip->type == T_DIR && !isdirempty(ip)) {
        iunlockput(ip);
        iunlockput(dp);
        end_op();
        return -1;
    }
    
    memset(&de, 0, sizeof(de));
    if(writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
        panic("unlink: writei");
    if(ip->type == T_DIR) {
        dp->nlink--;
        iupdate(dp);
    }
    iunlockput(dp);
    
    ip->nlink--;
    iupdate(ip);
    iunlockput(ip);
    
    end_op();
    
    return 0;
}

// fstat - 获取打开文件的stat信息
uint64 sys_fstat(void) {
    struct File *f;
    uint64 st_addr;
    struct stat st;
    int fd;
    
    arg_int(0, &fd);
    arg_uint64(1, &st_addr);
    
    if(fd < 0 || fd >= NOFILE || (f = myproc()->ofile[fd]) == 0)
        return -1;
    
    if(f->type != FILE_INODE)
        return -1;
    
    ilock(f->ip);
    stati(f->ip, &st);
    iunlock(f->ip);
    
    if(uvm_copyout(myproc()->pgtbl, (uint64)st_addr, (uint64)&st, sizeof(st)) < 0)
        return -1;
    
    return 0;
}

// dup - 复制文件描述符
uint64 sys_dup(void) {
    struct File *f;
    int fd, newfd;
    
    arg_int(0, &fd);
    
    if(fd < 0 || fd >= NOFILE || (f = myproc()->ofile[fd]) == 0)
        return -1;
    
    if((newfd = fdalloc(f)) < 0)
        return -1;
    
    file_dup(f);
    return newfd;
}

// link - 创建硬链接
uint64 sys_link(void) {
    char oldpath[128], newpath[128], name[DIRSIZ];
    struct inode *ip, *dp;
    uint64 oldpath_ptr, newpath_ptr;
    
    arg_uint64(0, &oldpath_ptr);
    arg_uint64(1, &newpath_ptr);
    
    if(uvm_copyin(myproc()->pgtbl, (uint64)oldpath, (uint64)oldpath_ptr, sizeof(oldpath)) < 0)
        return -1;
    if(uvm_copyin(myproc()->pgtbl, (uint64)newpath, (uint64)newpath_ptr, sizeof(newpath)) < 0)
        return -1;
    
    oldpath[127] = 0;
    newpath[127] = 0;
    
    begin_op();
    
    if((ip = namei(oldpath)) == 0) {
        end_op();
        return -1;
    }
    
    ilock(ip);
    
    if(ip->type == T_DIR) {
        iunlockput(ip);
        end_op();
        return -1;
    }
    
    ip->nlink++;
    iupdate(ip);
    iunlock(ip);
    
    if((dp = nameiparent(newpath, name)) == 0)
        goto bad;
    
    ilock(dp);
    
    if(dp->dev != ip->dev || dirlink(dp, name, ip->inum) < 0) {
        iunlockput(dp);
        goto bad;
    }
    
    iunlockput(dp);
    iput(ip);
    
    end_op();
    return 0;

bad:
    ilock(ip);
    ip->nlink--;
    iupdate(ip);
    iunlockput(ip);
    end_op();
    return -1;
}