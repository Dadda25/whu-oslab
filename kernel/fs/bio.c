/*
 * 块缓冲区管理模块
 * 实现磁盘块的缓存机制，使用LRU（最近最少使用）策略
 * 提供块的读取、写入、固定和释放操作
 */

#include "fs/bio.h"
#include "common.h"
#include "lib/print.h"
#include "lib/str.h"
#include "dev/virtio.h"

// 块缓冲区池大小
#define BUFFER_POOL_SIZE 30

// 块缓冲区管理结构
typedef struct {
    buf_t buffers[BUFFER_POOL_SIZE];  // 缓冲区数组
    buf_t lru_head;                    // LRU链表哨兵节点（最近使用的在head.next）
} buffer_cache_t;

static buffer_cache_t block_cache;

/*
 * 初始化块缓冲区子系统
 * 构建LRU双向链表，初始化virtio磁盘驱动
 */
void bio_init(void) {
    buf_t *current_buf;
    
    // 初始化LRU哨兵节点（形成环形链表）
    block_cache.lru_head.prev = &block_cache.lru_head;
    block_cache.lru_head.next = &block_cache.lru_head;
    
    // 将所有缓冲区插入到LRU链表
    // 新分配的块插入到头部后面（最近使用位置）
    for(current_buf = block_cache.buffers; 
        current_buf < block_cache.buffers + BUFFER_POOL_SIZE; 
        current_buf++) {
        
        current_buf->next = block_cache.lru_head.next;
        current_buf->prev = &block_cache.lru_head;
        block_cache.lru_head.next->prev = current_buf;
        block_cache.lru_head.next = current_buf;
    }
    
    // 初始化磁盘设备
    virtio_disk_init();
}

/*
 * 查找或分配缓冲区块
 * 参数：
 *   device_id: 设备号
 *   block_num: 块号
 * 返回：缓冲区指针
 * 
 * 查找策略：
 * 1. 先在缓存中查找是否已存在
 * 2. 如果不存在，从LRU链表尾部（最久未使用）获取空闲块
 */
static buf_t* buffer_get(uint64 device_id, uint64 block_num) {
    buf_t *current;
    
    // 验证块号有效性
    if(block_num >= FSSIZE) {
        printf("buffer_get: 块号 %d 超出范围（最大 %d）\n", 
               block_num, FSSIZE);
        panic("buffer_get: block number out of range");
    }
    
    // 第一遍扫描：查找是否已在缓存中
    for(current = block_cache.lru_head.next; 
        current != &block_cache.lru_head; 
        current = current->next) {
        
        if(current->dev == device_id && current->blockno == block_num) {
            current->refcnt++;
            return current;
        }
    }
    
    // 第二遍扫描：从尾部向前查找未使用的缓冲区
    // 尾部是最久未使用的块（LRU）
    for(current = block_cache.lru_head.prev; 
        current != &block_cache.lru_head; 
        current = current->prev) {
        
        if(current->refcnt == 0) {
            // 找到空闲缓冲区，重新初始化
            current->dev = device_id;
            current->blockno = block_num;
            current->valid = 0;
            current->refcnt = 1;
            return current;
        }
    }
    
    panic("buffer_get: no free buffers available");
    return 0;
}

/*
 * 读取磁盘块到缓冲区
 * 如果块已在缓存且有效，直接返回
 * 否则从磁盘读取
 */
buf_t* bread(uint64 device_id, uint64 block_num) {
    buf_t *buffer;
    
    buffer = buffer_get(device_id, block_num);
    
    // 如果缓冲区无效，需要从磁盘读取
    if(!buffer->valid) {
        virtio_disk_rw(buffer, 0);  // 0 表示读操作
        buffer->valid = 1;
    }
    
    return buffer;
}

/*
 * 将缓冲区写回磁盘
 */
void bwrite(buf_t *buffer) {
    if(buffer->refcnt < 1) {
        panic("bwrite: buffer not referenced");
    }
    
    virtio_disk_rw(buffer, 1);  // 1 表示写操作
}

/*
 * 释放缓冲区引用
 * 当引用计数归零时，将块移到LRU链表头部（MRU位置）
 * 这样最近使用的块在头部，最久未使用的在尾部
 */
void brelse(buf_t *buffer) {
    if(buffer->refcnt < 1) {
        panic("brelse: buffer not referenced");
    }
    
    buffer->refcnt--;
    
    // 引用计数归零时，移到MRU位置（头部）
    if(buffer->refcnt == 0) {
        // 从当前位置移除
        buffer->next->prev = buffer->prev;
        buffer->prev->next = buffer->next;
        
        // 插入到头部（最近使用位置）
        buffer->next = block_cache.lru_head.next;
        buffer->prev = &block_cache.lru_head;
        block_cache.lru_head.next->prev = buffer;
        block_cache.lru_head.next = buffer;
    }
}

/*
 * 固定缓冲区（增加引用计数）
 * 用于日志系统，防止缓冲区被回收
 */
void bpin(buf_t *buffer) {
    buffer->refcnt++;
}

/*
 * 解除固定（减少引用计数）
 */
void bunpin(buf_t *buffer) {
    buffer->refcnt--;
}