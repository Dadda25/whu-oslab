# 项目简介

本项目为操作系统实践的项目源代码，其参考了xv6的项目结构。

# 代码架构：

WHU-OSLAB  
├── include  
│   ├── dev 
│   │   ├── plic.h  
│   │   ├── timer.h  
│   │   └── uart.h  
│   ├── lib  
│   │   ├── lock.h  
│   │   ├── print.h  
│   │   └── lock.h  
│   ├── mem 
│   │   ├── mmap.h  
│   │   ├── pmem.h  
│   │   └── vmem.h 
│   ├── proc  
│   │   ├── cpu.h  
│   │   ├── initcode.h 
│   │   └── proc.h  
│   ├── syscall  
│   │   ├── syscall.h  
│   │   ├── sysfunc.h 
│   │   └── sysnum.h  
│   ├── trap
│   │   └── trap.h   
│   ├── common.h  
│   ├── memlayout.h  
│   └── riscv.h  
├── kernel  
│   ├── boot  
│   │   ├── main.c   
│   │   ├── start.c   
│   │   ├── entry.S  
│   │   └── Makefile  
│   ├── dev  
│   │   ├── uart.c  
│   │   ├── plic.c  
│   │   ├── timer.c  
│   │   └── Makefile  
│   ├── lib  
│   │   ├── print.c   
│   │   ├── spinlock.c 
│   │   ├── str.c 
│   │   └── Makefile    
│   ├── mem 
│   │   ├── kvm.c   
│   │   ├── mmap.c 
│   │   ├── pmem.c 
│   │   ├── uvm.c 
│   │   └── Makefile 
│   ├── proc  
│   │   ├── cpu.c 
│   │   ├── proc.c 
│   │   ├── swtch.S
│   │   └── Makefile  
│   ├── syscall 
│   │   ├── syscall.c  
│   │   ├── sysfunc.c   
│   │   └── Makefile  
│   ├── trap  
│   │   ├── trap_kernel.c 
│   │   ├── trap_user.c 
│   │   ├── trap.S
│   │   ├── trampoline.S
│   │   └── Makefile  
│   ├── Makefile  
│   └── kernel.ld  
├── user
│   ├── initcode.c 
│   ├── sys.h 
│   ├── syscall_arch.h
│   ├── syscall_num.h
│   └── Makefile 
├── Makefile  
├── .gitignore
├── .gdbinit.tmpl-riscv
├── LICENSE
├── README.md
└── common.mk  

# 使用方法

清除构建Makefile：

```bash
make clean
```

构建项目并执行：

```bash
make qemu
```