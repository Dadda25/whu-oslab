#include "riscv.h"
#include "dev/plic.h"
#include "lib/print.h"
#include "lib/str.h"
#include "mem/pmem.h"
#include "mem/vmem.h"
#include "proc/proc.h"
#include "trap/trap.h"
#include "fs/file.h"
#include "fs/bio.h"
#include "fs/fs.h"
#include "dev/vio.h"

volatile static int started = 0;

void main()
{
    int cpuid = r_tp();
    if(cpuid == 0){
        print_init();
        pmem_init();
        kvm_init();
        kvm_inithart();
        proc_init();
        trap_kernel_init();
        trap_kernel_inithart();
        plic_init();
        plic_inithart();
        fs_init(ROOTDEV);
        file_init();
        proc_make_first();

        printf("cpu %d is booting!\n", cpuid);
        __sync_synchronize();
        started = 1;

    }
    else{
        while(started == 0) {};
        __sync_synchronize();
        printf("cpu %d is booting!\n", cpuid);
        kvm_inithart();
        trap_kernel_inithart();
        plic_inithart();
    }

    proc_scheduler();
}