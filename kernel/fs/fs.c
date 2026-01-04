/*
 * 文件系统核心实现
 * 提供inode管理、目录操作、路径解析等核心功能
 */

#include "common.h"
#include "fs/bio.h"
#include "fs/log.h"
#include "fs/fs.h"
#include "mem/vmem.h"
#include "proc/cpu.h"
#include "lib/print.h"
#include "lib/str.h"

// 全局超级块
struct superblock sb;

// inode 缓存表
typedef struct {
    struct inode entries[NINODE];
} inode_cache_t;

static inode_cache_t inode_table;

/*
 * 文件系统初始化
 * 读取或创建超级块，初始化日志系统
 */
void fs_init(int dev) {
    struct buf *super_block;
    
    printf("[fs_init] 启动文件系统，设备=%d\n", (int)dev);
    bio_init();
    printf("[fs_init] 块缓冲区初始化完成\n");
    
    // 从磁盘读取超级块（位于块1）
    super_block = bread(dev, 1);
    printf("[fs_init] 超级块读取完成\n");
    memmove(&sb, super_block->data, sizeof(sb));
    brelse(super_block);
    printf("[fs_init] 超级块解析完成，魔数=0x%x\n", (int)sb.magic);
    
    // 检查魔数，如果不匹配则创建新文件系统
    if(sb.magic != FSMAGIC) {
        printf("fs_init: 检测到未格式化的磁盘，开始创建文件系统\n");
        
        // 配置文件系统布局
        sb.magic = FSMAGIC;
        sb.size = FSSIZE;
        sb.nblocks = FSSIZE - 100;  // 为元数据预留空间
        sb.ninodes = 200;
        sb.nlog = 30;
        sb.logstart = 2;
        sb.inodestart = 2 + sb.nlog;
        sb.bmapstart = sb.inodestart + sb.ninodes / IPB + 1;
        
        // 将超级块写入磁盘
        super_block = bread(dev, 1);
        memmove(super_block->data, &sb, sizeof(sb));
        bwrite(super_block);
        brelse(super_block);
        
        printf("fs_init: 超级块已写入\n");
        printf("  总块数=%d 数据块=%d inode数=%d\n", 
               (int)sb.size, (int)sb.nblocks, (int)sb.ninodes);
        
        printf("[fs_init] 初始化日志系统...\n");
        log_init(dev, &sb);
        printf("[fs_init] 日志系统初始化完成\n");
        
        // 初始化位图，标记系统块为已使用
        printf("fs_init: 标记系统块 0-%d 为已使用\n", (int)sb.bmapstart);
        
        printf("[fs_init] 开始文件系统事务...\n");
        begin_op();
        printf("[fs_init] 事务已开始\n");
        
        // 标记系统使用的块
        super_block = bread(dev, sb.bmapstart);
        for(int block = 0; block <= sb.bmapstart; block++) {
            int bit_index = block % BPB;
            int mask = 1 << (bit_index % 8);
            super_block->data[bit_index / 8] |= mask;
        }
        log_write(super_block);
        brelse(super_block);
        end_op();
        
        // 创建根目录
        struct inode *root_dir;
        begin_op();
        root_dir = ialloc(dev, T_DIR);
        if(root_dir->inum != ROOTINO) {
            panic("fs_init: 根目录inode号不是ROOTINO");
        }
        ilock(root_dir);
        root_dir->nlink = 2;  // . 和 ..
        root_dir->size = 0;
        iupdate(root_dir);
        
        // 添加 . 和 .. 目录项
        dirlink(root_dir, ".", ROOTINO);
        dirlink(root_dir, "..", ROOTINO);
        iunlockput(root_dir);
        end_op();
        
        printf("fs_init: 根目录创建完成\n\n");
    } else {
        log_init(dev, &sb);
    }
}

/*
 * 分配一个空闲磁盘块
 * 使用位图管理空闲块
 */
int fs_alloc(uint64 dev) {
    struct buf *bitmap_block;
    int block_num, bit_pos, bitmask;
    
    bitmap_block = 0;
    
    // 遍历所有数据块
    for(block_num = 0; block_num < sb.nblocks; block_num += BPB) {
        bitmap_block = bread(dev, BBLOCK(block_num, sb));
        
        // 在当前位图块中查找空闲位
        for(bit_pos = 0; 
            bit_pos < BPB && block_num + bit_pos < sb.nblocks; 
            bit_pos++) {
            
            bitmask = 1 << (bit_pos % 8);
            
            // 找到空闲块
            if((bitmap_block->data[bit_pos / 8] & bitmask) == 0) {
                // 标记为已使用
                bitmap_block->data[bit_pos / 8] |= bitmask;
                log_write(bitmap_block);
                brelse(bitmap_block);
                
                // 清空块内容
                struct buf *zero_block = bread(dev, block_num + bit_pos);
                memset(zero_block->data, 0, BSIZE);
                log_write(zero_block);
                brelse(zero_block);
                
                return block_num + bit_pos;
            }
        }
        brelse(bitmap_block);
    }
    
    panic("fs_alloc: 磁盘空间不足");
    return 0;
}

/*
 * 释放磁盘块
 */
void fs_free(int dev, uint64 block_num) {
    struct buf *bitmap_block;
    int bit_index, bitmask;
    
    bitmap_block = bread(dev, BBLOCK(block_num, sb));
    bit_index = block_num % BPB;
    bitmask = 1 << (bit_index % 8);
    
    // 检查块是否真的在使用中
    if((bitmap_block->data[bit_index / 8] & bitmask) == 0) {
        panic("fs_free: 尝试释放未分配的块");
    }
    
    // 清除位图中的标记
    bitmap_block->data[bit_index / 8] &= ~bitmask;
    log_write(bitmap_block);
    brelse(bitmap_block);
}

/*
 * 分配一个新的inode
 * 在磁盘上查找类型为0的inode
 */
struct inode* ialloc(uint64 dev, short file_type) {
    int inode_num;
    struct buf *inode_block;
    struct dinode *disk_inode;
    struct inode *mem_inode;
    
    // 遍历所有inode（从1开始，0保留）
    for(inode_num = 1; inode_num < sb.ninodes; inode_num++) {
        inode_block = bread(dev, IBLOCK(inode_num, sb));
        disk_inode = (struct dinode*)inode_block->data + inode_num % IPB;
        
        // 找到空闲inode
        if(disk_inode->type == 0) {
            // 初始化磁盘inode
            memset((addr_t)disk_inode, 0, sizeof(*disk_inode));
            disk_inode->type = file_type;
            log_write(inode_block);
            brelse(inode_block);
            
            // 获取内存中的inode表示
            mem_inode = iget(dev, inode_num);
            
            // 重要：不立即设置valid，让ilock来加载
            // 但需要初始化基本字段
            ilock(mem_inode);  // 这会从磁盘加载
            iunlock(mem_inode);
            
            return mem_inode;
        }
        brelse(inode_block);
    }
    
    panic("ialloc: 没有可用的inode");
    return 0;
}

/*
 * 获取inode的内存表示
 * 如果已在缓存中则返回，否则分配新的缓存项
 */
struct inode* iget(uint64 dev, uint64 inode_num) {
    struct inode *current, *free_slot;
    
    free_slot = 0;
    
    // 在缓存中查找
    for(current = &inode_table.entries[0]; 
        current < &inode_table.entries[NINODE]; 
        current++) {
        
        // 找到已缓存的inode
        if(current->ref > 0 && current->dev == dev && current->inum == inode_num) {
            current->ref++;
            return current;
        }
        
        // 记录第一个空闲槽位
        if(free_slot == 0 && current->ref == 0) {
            free_slot = current;
        }
    }
    
    // 未找到，需要分配新缓存项
    if(free_slot == 0) {
        panic("iget: inode缓存已满");
    }
    
    current = free_slot;
    current->dev = dev;
    current->inum = inode_num;
    current->ref = 1;
    current->valid = 0;  // 延迟加载，等待ilock
    
    return current;
}

/*
 * 增加inode引用计数
 */
struct inode* idup(struct inode *inode_ptr) {
    inode_ptr->ref++;
    return inode_ptr;
}

/*
 * 锁定inode并从磁盘加载数据（如果需要）
 */
void ilock(struct inode *inode_ptr) {
    struct buf *inode_block;
    struct dinode *disk_inode;
    
    if(inode_ptr == 0 || inode_ptr->ref < 1) {
        panic("ilock: 无效的inode");
    }
    
    // 如果数据未加载，从磁盘读取
    if(inode_ptr->valid == 0) {
        inode_block = bread(inode_ptr->dev, IBLOCK(inode_ptr->inum, sb));
        disk_inode = (struct dinode*)inode_block->data + inode_ptr->inum % IPB;
        
        // 复制磁盘inode到内存
        inode_ptr->type = disk_inode->type;
        inode_ptr->major = disk_inode->major;
        inode_ptr->minor = disk_inode->minor;
        inode_ptr->nlink = disk_inode->nlink;
        inode_ptr->size = disk_inode->size;
        memmove(inode_ptr->addrs, disk_inode->addrs, sizeof(inode_ptr->addrs));
        
        brelse(inode_block);
        inode_ptr->valid = 1;
        
        // 验证类型非零
        if(inode_ptr->type == 0) {
            printf("ilock: inode %d (dev %d) 类型为0\n", 
                   (int)inode_ptr->inum, (int)inode_ptr->dev);
            panic("ilock: inode类型无效");
        }
    }
}

/*
 * 解锁inode
 */
void iunlock(struct inode *inode_ptr) {
    if(inode_ptr == 0 || inode_ptr->ref < 1) {
        panic("iunlock: 无效的inode");
    }
}

/*
 * 将inode写回磁盘
 */
void iupdate(struct inode *inode_ptr) {
    struct buf *inode_block;
    struct dinode *disk_inode;
    
    inode_block = bread(inode_ptr->dev, IBLOCK(inode_ptr->inum, sb));
    disk_inode = (struct dinode*)inode_block->data + inode_ptr->inum % IPB;
    
    // 复制内存inode到磁盘
    disk_inode->type = inode_ptr->type;
    disk_inode->major = inode_ptr->major;
    disk_inode->minor = inode_ptr->minor;
    disk_inode->nlink = inode_ptr->nlink;
    disk_inode->size = inode_ptr->size;
    memmove(disk_inode->addrs, inode_ptr->addrs, sizeof(inode_ptr->addrs));
    
    log_write(inode_block);
    brelse(inode_block);
}

/*
 * 释放inode引用
 * 如果是最后一个引用且无链接，则回收inode
 */
void iput(struct inode *inode_ptr) {
    // 如果是根目录的最后一个引用，保持活跃
    if(inode_ptr->inum == ROOTINO) {
        inode_ptr->ref--;
        if(inode_ptr->ref > 0) {
            return;
        }
        // 根目录即使ref为0也不回收，只是减少引用
        return;
    }
    
    // 如果是最后一个引用且无硬链接，回收inode
    if(inode_ptr->ref == 1 && inode_ptr->valid && inode_ptr->nlink == 0) {
        itrunc(inode_ptr);
        inode_ptr->type = 0;
        iupdate(inode_ptr);
        inode_ptr->valid = 0;
    }
    
    inode_ptr->ref--;
}

/*
 * 解锁并释放inode
 */
void iunlockput(struct inode *inode_ptr) {
    iunlock(inode_ptr);
    iput(inode_ptr);
}

/*
 * 将文件偏移量映射到磁盘块号
 * 支持直接块和一级间接块
 */
static uint64 block_map(struct inode *inode_ptr, uint64 logical_block) {
    uint32 physical_addr;
    uint32 *indirect_block;
    struct buf *block_buf;
    
    // 直接块
    if(logical_block < NDIRECT) {
        if((physical_addr = inode_ptr->addrs[logical_block]) == 0) {
            inode_ptr->addrs[logical_block] = physical_addr = fs_alloc(inode_ptr->dev);
        }
        return physical_addr;
    }
    
    // 间接块
    logical_block -= NDIRECT;
    
    if(logical_block < NINDIRECT) {
        // 分配间接块（如果需要）
        if((physical_addr = inode_ptr->addrs[NDIRECT]) == 0) {
            inode_ptr->addrs[NDIRECT] = physical_addr = fs_alloc(inode_ptr->dev);
        }
        
        block_buf = bread(inode_ptr->dev, physical_addr);
        indirect_block = (uint32*)block_buf->data;
        
        if((physical_addr = indirect_block[logical_block]) == 0) {
            indirect_block[logical_block] = physical_addr = fs_alloc(inode_ptr->dev);
            log_write(block_buf);
        }
        
        brelse(block_buf);
        return physical_addr;
    }
    
    panic("block_map: 块号超出范围");
    return 0;
}

/*
 * 截断文件，释放所有数据块
 */
void itrunc(struct inode *inode_ptr) {
    int idx, j;
    struct buf *indirect_buf;
    uint32 *indirect_table;
    
    // 释放直接块
    for(idx = 0; idx < NDIRECT; idx++) {
        if(inode_ptr->addrs[idx]) {
            fs_free(inode_ptr->dev, inode_ptr->addrs[idx]);
            inode_ptr->addrs[idx] = 0;
        }
    }
    
    // 释放间接块
    if(inode_ptr->addrs[NDIRECT]) {
        indirect_buf = bread(inode_ptr->dev, inode_ptr->addrs[NDIRECT]);
        indirect_table = (uint32*)indirect_buf->data;
        
        for(j = 0; j < NINDIRECT; j++) {
            if(indirect_table[j]) {
                fs_free(inode_ptr->dev, indirect_table[j]);
            }
        }
        
        brelse(indirect_buf);
        fs_free(inode_ptr->dev, inode_ptr->addrs[NDIRECT]);
        inode_ptr->addrs[NDIRECT] = 0;
    }
    
    inode_ptr->size = 0;
    iupdate(inode_ptr);
}

/*
 * 从inode读取数据
 */
int readi(struct inode *inode_ptr, int to_user, uint64 dest, uint64 offset, uint64 count) {
    uint64 total_read, chunk_size;
    struct buf *block_buf;
    
    // 检查读取范围
    if(offset > inode_ptr->size || offset + count < offset) {
        return 0;
    }
    if(offset + count > inode_ptr->size) {
        count = inode_ptr->size - offset;
    }
    
    // 逐块读取
    for(total_read = 0; total_read < count; 
        total_read += chunk_size, offset += chunk_size, dest += chunk_size) {
        
        block_buf = bread(inode_ptr->dev, block_map(inode_ptr, offset / BSIZE));
        chunk_size = BSIZE - offset % BSIZE;
        if(count - total_read < chunk_size) {
            chunk_size = count - total_read;
        }
        
        if(to_user) {
            // 复制到用户空间
            if(uvm_copyout(myproc()->pgtbl, dest, 
                          (uint64)(block_buf->data + offset % BSIZE), 
                          chunk_size) < 0) {
                brelse(block_buf);
                return -1;
            }
        } else {
            // 复制到内核空间
            memmove((void*)dest, block_buf->data + offset % BSIZE, chunk_size);
        }
        
        brelse(block_buf);
    }
    
    return count;
}

/*
 * 向inode写入数据
 */
int writei(struct inode *inode_ptr, int from_user, uint64 source, uint64 offset, uint64 count) {
    uint64 total_written, chunk_size;
    struct buf *block_buf;
    
    // 检查写入范围
    if(offset > inode_ptr->size || offset + count < offset) {
        return -1;
    }
    if(offset + count > MAXFILE * BSIZE) {
        return -1;
    }
    
    // 逐块写入
    for(total_written = 0; total_written < count; 
        total_written += chunk_size, offset += chunk_size, source += chunk_size) {
        
        block_buf = bread(inode_ptr->dev, block_map(inode_ptr, offset / BSIZE));
        chunk_size = BSIZE - offset % BSIZE;
        if(count - total_written < chunk_size) {
            chunk_size = count - total_written;
        }
        
        if(from_user) {
            // 从用户空间复制
            if(uvm_copyin(myproc()->pgtbl, 
                         (uint64)(block_buf->data + offset % BSIZE), 
                         source, chunk_size) < 0) {
                brelse(block_buf);
                return -1;
            }
        } else {
            // 从内核空间复制
            memmove(block_buf->data + offset % BSIZE, (void*)source, chunk_size);
        }
        
        log_write(block_buf);
        brelse(block_buf);
    }
    
    // 更新文件大小
    if(offset > inode_ptr->size) {
        inode_ptr->size = offset;
    }
    
    iupdate(inode_ptr);
    
    return count;
}

/*
 * 在目录中查找文件
 */
struct inode* dirlookup(struct inode *dir_inode, char *filename, uint64 *offset_out) {
    uint64 current_offset;
    struct dirent entry;
    
    if(dir_inode->type != T_DIR) {
        panic("dirlookup: 不是目录类型");
    }
    
    // 遍历目录项
    for(current_offset = 0; current_offset < dir_inode->size; current_offset += sizeof(entry)) {
        if(readi(dir_inode, 0, (uint64)&entry, current_offset, sizeof(entry)) != sizeof(entry)) {
            panic("dirlookup: 读取目录项失败");
        }
        
        if(entry.inum == 0) {
            continue;  // 跳过空目录项
        }
        
        if(strncmp(filename, entry.name, DIRSIZ) == 0) {
            if(offset_out) {
                *offset_out = current_offset;
            }
            return iget(dir_inode->dev, entry.inum);
        }
    }
    
    return 0;
}

/*
 * 在目录中添加目录项
 */
int dirlink(struct inode *dir_inode, char *filename, uint64 inode_num) {
    int current_offset;
    struct dirent entry;
    struct inode *existing;
    
    // 检查文件名是否已存在
    if((existing = dirlookup(dir_inode, filename, 0)) != 0) {
        iput(existing);
        return -1;
    }
    
    // 查找空闲目录项
    for(current_offset = 0; current_offset < dir_inode->size; current_offset += sizeof(entry)) {
        if(readi(dir_inode, 0, (uint64)&entry, current_offset, sizeof(entry)) != sizeof(entry)) {
            panic("dirlink: 读取目录项失败");
        }
        if(entry.inum == 0) {
            break;  // 找到空闲项
        }
    }
    
    // 填充目录项
    strncpy(entry.name, filename, DIRSIZ);
    entry.inum = inode_num;
    
    if(writei(dir_inode, 0, (uint64)&entry, current_offset, sizeof(entry)) != sizeof(entry)) {
        panic("dirlink: 写入目录项失败");
    }
    
    return 0;
}

/*
 * 跳过路径中的斜杠并提取下一个元素
 */
static char* extract_path_element(char *path, char *element) {
    char *start;
    int length;
    
    // 跳过前导斜杠
    while(*path == '/') {
        path++;
    }
    
    if(*path == 0) {
        return 0;
    }
    
    start = path;
    
    // 查找路径元素结束位置
    while(*path != '/' && *path != 0) {
        path++;
    }
    
    length = path - start;
    if(length >= DIRSIZ) {
        length = DIRSIZ - 1;
    }
    
    memmove(element, start, length);
    element[length] = 0;
    
    // 跳过尾随斜杠
    while(*path == '/') {
        path++;
    }
    
    return path;
}

/*
 * 路径解析核心函数
 * parent_mode: 1表示返回父目录，0表示返回最终inode
 */
static struct inode* path_walk(char *path, int parent_mode, char *final_name) {
    struct inode *current, *next_inode;
    
    // 确定起始目录
    if(*path == '/') {
        current = iget(ROOTDEV, ROOTINO);
    } else {
        current = idup(iget(ROOTDEV, ROOTINO));
    }
    
    // 逐级解析路径
    while((path = extract_path_element(path, final_name)) != 0) {
        ilock(current);
        
        if(current->type != T_DIR) {
            iunlockput(current);
            return 0;
        }
        
        // 如果请求父目录且已到达最后一个元素
        if(parent_mode && *path == '\0') {
            iunlock(current);
            return current;
        }
        
        // 查找下一级目录/文件
        if((next_inode = dirlookup(current, final_name, 0)) == 0) {
            iunlockput(current);
            return 0;
        }
        
        iunlockput(current);
        current = next_inode;
    }
    
    if(parent_mode) {
        iput(current);
        return 0;
    }
    
    return current;
}

/*
 * 查找路径对应的inode
 */
struct inode* namei(char *path) {
    char element[DIRSIZ];
    return path_walk(path, 0, element);
}

/*
 * 查找路径的父目录inode
 */
struct inode* nameiparent(char *path, char *final_name) {
    return path_walk(path, 1, final_name);
}

/*
 * 将inode信息复制到stat结构
 */
void stati(struct inode *inode_ptr, struct stat *stat_buf) {
    stat_buf->dev = inode_ptr->dev;
    stat_buf->ino = inode_ptr->inum;
    stat_buf->type = inode_ptr->type;
    stat_buf->nlink = inode_ptr->nlink;
    stat_buf->size = inode_ptr->size;
}