// 标准输出和报错机制
#include <stdarg.h>
#include "lib/print.h"
#include "lib/lock.h"
#include "dev/uart.h"

volatile int panicked = 0;

// 防止占用的lock
static struct {
  struct spinlock print_lk;
  int locking;
} pr;

static char digits[] = "0123456789abcdef";

void print_init(void)
{
  uart_init();
  spinlock_init(&pr.print_lk, "pr");
  pr.locking = 1;
}

// 辅助函数
static void
printint(int64 xx, int base, int sign)
{
  char buf[16];
  int i;

  // 这里调整，加大位宽，防止极端情况
  int64 x = xx;

  if(sign && (sign = xx < (int64)0))
    x = -x;

  i = 0;
  do {
    buf[i++] = digits[x % base];
  } while((x /= base) != 0);

  if(sign)
    buf[i++] = '-';

  while(--i >= 0)
    uart_putc_sync(buf[i]);
}

static void
printptr(uint64 x)
{
  int i;
  uart_putc_sync('0');
  uart_putc_sync('x');
  for (i = 0; i < (sizeof(uint64) * 2); i++, x <<= 4)
    uart_putc_sync(digits[x >> (sizeof(uint64) * 8 - 4)]);
}

// 将格式化字符输出到中断。支持%d, %x, %p, %s, %c。
void printf(const char *fmt, ...)
{
  va_list ap;
  int64 i;
  int c, locking;
  char *s;

  locking = pr.locking;
  if(locking)
    spinlock_acquire(&pr.print_lk);

  if (fmt == 0)
    panic("null fmt");

  va_start(ap, fmt);
  for(i = 0; (c = fmt[i] & 0xff) != 0; i++){
    if(c != '%'){
      uart_putc_sync(c);
      continue;
    }
    c = fmt[++i] & 0xff;
    if(c == 0)
      break;
    switch(c){
    case 'd':
      printint(va_arg(ap, int), 10, 1);
      break;
    case 'x':
      printint(va_arg(ap, int), 16, 1);
      break;
    case 'p':
      printptr(va_arg(ap, uint64));
      break;
    case 's':
      if((s = va_arg(ap, char*)) == 0)
        s = "(null)";
      for(; *s; s++)
        uart_putc_sync(*s);
      break;
    case 'c':
      uart_putc_sync(va_arg(ap, int));
      break;
    case '%':
      uart_putc_sync('%');
      break;
    default:
      // Print unknown % sequence to draw attention.
      uart_putc_sync('%');
      uart_putc_sync(c);
      break;
    }
  }
  va_end(ap);

  if(locking)
    spinlock_release(&pr.print_lk);
}

void panic(const char *s)
{
  pr.locking = 0;
  printf("panic: ");
  printf(s);
  printf("\n");
  panicked = 1; // freeze uart output from other CPUs
  for(;;)
    ;
}

void assert(bool condition, const char* warning)
{
    if(!condition){
        panic(warning);
    }
}

// 清屏函数实现
void clear_screen(void) {
    // 发送ANSI转义序列: \033 是 ESC 的八进制表示
    // [2J 表示清除整个屏幕
    uart_puts("\033[2J"); 
    // [H 表示将光标移动到左上角
    uart_puts("\033[H");

    printf("Screen cleared\n");
}