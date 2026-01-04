#ifndef _STDARG_H
#define _STDARG_H

// RISC-V 64位架构的可变参数实现
// 使用 GCC 内置函数以确保正确的调用约定处理

typedef __builtin_va_list va_list;

// va_start - 初始化 va_list，使其指向第一个可变参数
#define va_start(AP, LASTARG) __builtin_va_start(AP, LASTARG)

// va_arg - 获取下一个可变参数
#define va_arg(AP, TYPE) __builtin_va_arg(AP, TYPE)

// va_end - 清理 va_list
#define va_end(AP) __builtin_va_end(AP)

// va_copy - 复制 va_list
#define va_copy(DEST, SRC) __builtin_va_copy(DEST, SRC)

#endif /* _STDARG_H */
