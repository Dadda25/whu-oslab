/*
 * 文件系统日志模块
 * 实现写前日志(Write-Ahead Logging)以支持崩溃恢复
 * 
 * 日志协议：
 * 1. write_log() - 将修改的块写入日志区
 * 2. write_head() - 写入日志头到磁盘（提交点）
 * 3. install_trans() - 将日志块安装到实际位置
 * 4. write_head() - 清除日志头（事务完成）
 */

#include "fs/log.h"
#include "common.h"
#include "fs/bio.h"
#include "fs/fs.h"
#include "lib/print.h"
#include "lib/str.h"
#include "lib/lock.h"

#define MAX_LOG_BLOCKS 30

// 日志头结构（存储在日志起始块）
typedef struct {
    int block_count;                    // 日志中的块数
    int block_addrs[MAX_LOG_BLOCKS];    // 块地址数组
} log_header_t;

// 日志管理结构
typedef struct {
    spinlock_t lock;                 // 保护日志数据结构的锁
    int device_id;                   // 日志所在设备
    int start_block;                 // 日志起始块号
    int total_blocks;                // 日志总块数
    int active_ops;                  // 当前活跃的文件系统操作数
    int is_committing;               // 是否正在提交事务
    log_header_t header;             // 内存中的日志头
} log_context_t;

static log_context_t log_sys;

// 内部函数声明
static void recover_log(void);
static void commit_transaction(void);
static void write_log_blocks(void);
static void write_log_header(void);
static void install_log_blocks(int is_recovery);
static void read_log_header(void);

/*
 * 初始化日志系统
 * 清空日志区并执行崩溃恢复
 */
void log_init(int device, struct superblock *super_block) {
    if(sizeof(log_header_t) >= BSIZE) {
        panic("log_init: 日志头过大");
    }
    
    // 初始化锁
    spinlock_init(&log_sys.lock, "log");
    
    log_sys.device_id = device;
    log_sys.start_block = super_block->logstart;
    log_sys.total_blocks = super_block->nlog;
    log_sys.active_ops = 0;
    log_sys.is_committing = 0;
    
    // 清空日志区，但使用日志写入而非直接写入
    // 这里我们先读取日志头，如果有未完成的事务则恢复
    // 然后再清空
    
    recover_log();
}

/*
 * 从磁盘读取日志头到内存
 */
static void read_log_header(void) {
    struct buf *header_block = bread(log_sys.device_id, log_sys.start_block);
    log_header_t *disk_header = (log_header_t *)(header_block->data);
    int i;
    
    log_sys.header.block_count = disk_header->block_count;
    for(i = 0; i < log_sys.header.block_count; i++) {
        log_sys.header.block_addrs[i] = disk_header->block_addrs[i];
    }
    brelse(header_block);
}

/*
 * 将内存中的日志头写入磁盘
 * 这是事务的提交点：在此之前崩溃，事务无效；在此之后崩溃，事务有效
 */
static void write_log_header(void) {
    struct buf *header_block = bread(log_sys.device_id, log_sys.start_block);
    log_header_t *disk_header = (log_header_t *)(header_block->data);
    int i;
    
    disk_header->block_count = log_sys.header.block_count;
    for(i = 0; i < log_sys.header.block_count; i++) {
        disk_header->block_addrs[i] = log_sys.header.block_addrs[i];
    }
    bwrite(header_block);
    brelse(header_block);
}

/*
 * 将修改的块从缓存复制到日志区
 */
static void write_log_blocks(void) {
    int block_idx;
    
    for(block_idx = 0; block_idx < log_sys.header.block_count; block_idx++) {
        // 读取日志块
        struct buf *log_block = bread(log_sys.device_id, 
                                      log_sys.start_block + block_idx + 1);
        // 读取缓存中的源块
        struct buf *src_block = bread(log_sys.device_id, 
                                      log_sys.header.block_addrs[block_idx]);
        
        // 复制数据
        memmove(log_block->data, src_block->data, BSIZE);
        bwrite(log_block);
        
        brelse(src_block);
        brelse(log_block);
    }
}

/*
 * 将日志中的块安装到文件系统的实际位置
 * is_recovery: 如果是恢复模式则为1，否则为0
 */
static void install_log_blocks(int is_recovery) {
    int block_idx;
    
    for(block_idx = 0; block_idx < log_sys.header.block_count; block_idx++) {
        // 读取日志块
        struct buf *log_block = bread(log_sys.device_id, 
                                      log_sys.start_block + block_idx + 1);
        // 读取目标块
        struct buf *dest_block = bread(log_sys.device_id, 
                                       log_sys.header.block_addrs[block_idx]);
        
        // 复制数据
        memmove(dest_block->data, log_block->data, BSIZE);
        bwrite(dest_block);
        
        // 如果不是恢复模式，需要解除固定
        if(!is_recovery) {
            bunpin(dest_block);
        }
        
        brelse(log_block);
        brelse(dest_block);
    }
}

/*
 * 从日志恢复文件系统
 * 在系统启动时调用，在第一次用户进程运行之前
 */
static void recover_log(void) {
    read_log_header();
    
    // 如果有未提交的事务，安装它们
    if(log_sys.header.block_count > 0) {
        install_log_blocks(1);
    }
    
    // 清除日志
    log_sys.header.block_count = 0;
    write_log_header();
}

/*
 * 开始文件系统操作
 * 等待直到有足够的日志空间
 */
void begin_op(void) {
    spinlock_acquire(&log_sys.lock);
    
    while(1) {
        if(log_sys.is_committing) {
            // 等待提交完成
            spinlock_release(&log_sys.lock);
            // 简单的等待，实际可以用sleep/wakeup优化
            spinlock_acquire(&log_sys.lock);
        } else if(log_sys.header.block_count + (log_sys.active_ops + 1) * MAX_LOG_BLOCKS 
                  > MAX_LOG_BLOCKS) {
            // 日志空间不足，等待
            spinlock_release(&log_sys.lock);
            spinlock_acquire(&log_sys.lock);
        } else {
            log_sys.active_ops++;
            spinlock_release(&log_sys.lock);
            break;
        }
    }
}

/*
 * 结束文件系统操作
 * 如果是最后一个操作，则提交事务
 */
void end_op(void) {
    int should_commit = 0;
    
    spinlock_acquire(&log_sys.lock);
    
    log_sys.active_ops--;
    
    if(log_sys.is_committing) {
        panic("end_op: 不应在提交时调用");
    }
    
    // 如果是最后一个操作，准备提交
    if(log_sys.active_ops == 0) {
        should_commit = 1;
        log_sys.is_committing = 1;
    }
    
    spinlock_release(&log_sys.lock);
    
    if(should_commit) {
        commit_transaction();
        
        spinlock_acquire(&log_sys.lock);
        log_sys.is_committing = 0;
        spinlock_release(&log_sys.lock);
    }
}

/*
 * 将修改后的块添加到日志
 * 在事务中调用
 */
void log_write(struct buf *block) {
    int i;
    
    spinlock_acquire(&log_sys.lock);
    
    // 检查日志空间
    if(log_sys.header.block_count >= MAX_LOG_BLOCKS || 
       log_sys.header.block_count >= log_sys.total_blocks - 1) {
        panic("log_write: 事务过大");
    }
    
    if(log_sys.active_ops < 1) {
        panic("log_write: 在事务外调用");
    }
    
    // 查找块是否已在日志中
    for(i = 0; i < log_sys.header.block_count; i++) {
        if(log_sys.header.block_addrs[i] == block->blockno) {
            break;
        }
    }
    
    // 如果是新块，添加到日志
    log_sys.header.block_addrs[i] = block->blockno;
    if(i == log_sys.header.block_count) {
        bpin(block);  // 防止块被回收
        log_sys.header.block_count++;
    }
    
    spinlock_release(&log_sys.lock);
}

/*
 * 提交当前事务
 * 完整的四步提交协议
 */
static void commit_transaction(void) {
    if(log_sys.header.block_count > 0) {
        write_log_blocks();         // 步骤1: 写入修改的块到日志
        write_log_header();         // 步骤2: 写入日志头（提交点）
        install_log_blocks(0);      // 步骤3: 安装日志到实际位置
        log_sys.header.block_count = 0;
        write_log_header();         // 步骤4: 清除日志头
    }
}