#include "sys.h"
#include "common.h"

void test_syscall(void)
{
    syscall(SYS_test);
}

int print(const char * str)
{
    return syscall(SYS_print, str);
}

int brk(int size)
{
    return syscall(SYS_open, size);
}

int open(char * dir, int state)
{
    return syscall(SYS_open, dir, state);
}

int close(int fd)
{
    return syscall(SYS_close, fd);
}

int fork()
{
    return syscall(SYS_fork);
}

int wait(int* addr)
{
    return syscall(SYS_wait, (uint64)addr);
}

int exit(int n)
{
    return syscall(SYS_exit, n);
}

int sleep(int n)
{
    return syscall(SYS_sleep, n);
}

int kill(int n)
{
    return syscall(SYS_kill, n);
}

int getpid(void)
{
    return syscall(SYS_getpid);
}

int read(int fd, char* buf, int sz)
{
    return syscall(SYS_read, fd, buf, sz);
}

int write(int fd, const char* msg, int len)
{
    return syscall(SYS_write, fd, msg, len);
}

int mkdir(char* dir)
{
    return syscall(SYS_mkdir, dir);
}

int link(char *f1, char* f2)
{
    return syscall(SYS_link, f1, f2);
}

int unlink(char * fd)
{
    return syscall(SYS_unlink, fd);
}

int fstat(int fs, char * addr)
{
    return syscall(SYS_fstat, fs, addr);
}

int dup(int fs)
{
    return syscall(SYS_dup, fs);
}

int yield(void)
{
    return syscall(SYS_yield);
}

uint64 get_time(void)
{
    return syscall(SYS_getticks);
}