#include "stdarg.h"
#include "user.h"
#include "common.h"

// 输出缓冲区大小
#define PRINT_BUF_SIZE 256

// 静态辅助函数声明
static void printint(long long xx, int base, int sign);
static void printptr(unsigned long long x);
static void printchar(char c);
static void flush_buf(void);

// 输出缓冲区
static char print_buf[PRINT_BUF_SIZE];
static int print_buf_pos = 0;

// printchar - 将字符添加到缓冲区，如果缓冲区满或遇到换行符则刷新
static void printchar(char c) {
    // 如果缓冲区快满了，先刷新
    if (print_buf_pos >= PRINT_BUF_SIZE - 1) {
        flush_buf();
    }
    
    // 添加字符到缓冲区
    print_buf[print_buf_pos++] = c;
    
    // 如果是换行符，立即刷新缓冲区
    if (c == '\n') {
        flush_buf();
    }
}

// flush_buf - 刷新缓冲区，将缓冲区内容通过系统调用打印
static void flush_buf(void) {
    if (print_buf_pos > 0) {
        print_buf[print_buf_pos] = '\0';
        syscall(SYS_print, print_buf);
        print_buf_pos = 0;
    }
}

// printint - 打印一个带符号的整数 (迭代实现，避免栈溢出)
// xx: 要打印的数字
// base: 进制 (例如 10 或 16)
// sign: 是否处理符号 (1 表示处理, 0 表示不处理)
static void printint(long long xx, int base, int sign) {
    char buf[32];
    int i = 0;
    unsigned long long x;

    // 处理负数，并解决 INT_MIN 溢出问题
    if (sign && xx < 0) {
        printchar('-');
        x = -xx;
    } else {
        x = xx;
    }

    // 使用迭代法将数字转换为字符串，逆序存入 buf
    do {
        buf[i++] = "0123456789abcdef"[x % base];
    } while ((x /= base) != 0);

    // 将 buf 中的字符正序输出
    while (--i >= 0) {
        printchar(buf[i]);
    }
}

// printptr - 打印一个指针地址
// 地址以 0x 开头的十六进制格式输出
static void printptr(unsigned long long x) {
    printchar('0');
    printchar('x');
    for (int i = 0; i < (sizeof(unsigned long long) * 2); i++, x <<= 4) {
        printchar("0123456789abcdef"[x >> (sizeof(unsigned long long) * 8 - 4)]);
    }
}


// printf - 用户态格式化输出主函数
void printf(const char *fmt, ...) {
    va_list ap;
    char *s;
    int c;

    if (fmt == 0) {
        return; // 处理空指针
    }

    va_start(ap, fmt);
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            printchar(*p);
            continue;
        }

        p++; // 跳过 '%'
        if (*p == '\0') {
             break; // 防止格式字符串以 '%' 结尾
        }

        // 处理长度修饰符 'l' (long)
        int is_long = 0;
        if (*p == 'l') {
            is_long = 1;
            p++;
            if (*p == '\0') {
                break; // 防止格式字符串以 '%l' 结尾
            }
        }

        switch (*p) {
            case 'd': // 整数
                if (is_long) {
                    printint(va_arg(ap, long), 10, 1);
                } else {
                    printint(va_arg(ap, int), 10, 1);
                }
                break;
            case 'u': // 无符号整数
                if (is_long) {
                    // %lu: unsigned long
                    printint(va_arg(ap, unsigned long), 10, 0);
                } else {
                    // %u: unsigned int
                    printint(va_arg(ap, unsigned int), 10, 0);
                }
                break;
            case 'x': // 十六进制
                if (is_long) {
                    printint(va_arg(ap, long), 16, 0);
                } else {
                    printint(va_arg(ap, int), 16, 0);
                }
                break;
            case 'p': // 指针
                printptr(va_arg(ap, unsigned long long));
                break;
            case 's': // 字符串
                if ((s = va_arg(ap, char *)) == 0) {
                    s = "(null)";
                }
                while (*s) {
                    printchar(*s++);
                }
                break;
            case 'c': // 字符
                c = va_arg(ap, int);
                printchar(c);
                break;
            case '%': // 百分号
                printchar('%');
                break;
            default: // 未知格式，直接打印
                printchar('%');
                if (is_long) {
                    printchar('l');
                }
                printchar(*p);
                break;
        }
    }
    va_end(ap);
    
    // 最后刷新缓冲区
    flush_buf();
}

void my_assert(bool condition, const char* warning)
{
    if(!condition){
        printf("Assert failed: ");
        printf(warning);
        while(1){}
    }
}