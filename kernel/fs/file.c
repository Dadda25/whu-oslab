/*
 * 文件描述符管理模块
 * 提供文件打开表管理和文件操作接口
 */

#include "common.h"
#include "fs/fs.h"
#include "fs/log.h"
#include "fs/file.h"
#include "lib/print.h"
#include "lib/str.h"

// 全局文件表
struct ftable_s ftable;

/*
 * 初始化文件表
 */
void file_init(void) {
    int i;
    
    for(i = 0; i < NFILE; i++) {
        ftable.file[i].ref = 0;
        ftable.file[i].type = FILE_NONE;
    }
}

/*
 * 分配一个文件结构
 * 从文件表中找到一个空闲项
 */
struct File* alloc_file(void) {
    int i;
    
    for(i = 0; i < NFILE; i++) {
        if(ftable.file[i].ref == 0) {
            // 初始化文件结构
            ftable.file[i].ref = 1;
            ftable.file[i].type = FILE_NONE;
            ftable.file[i].readable = 0;
            ftable.file[i].writable = 0;
            ftable.file[i].append = 0;
            ftable.file[i].ip = 0;
            ftable.file[i].off = 0;
            return &ftable.file[i];
        }
    }
    
    return 0;  // 无可用文件结构
}

/*
 * 增加文件引用计数（复制文件描述符）
 */
struct File* file_dup(struct File *file_ptr) {
    if(file_ptr->ref < 1) {
        panic("file_dup: 文件引用计数无效");
    }
    
    file_ptr->ref++;
    return file_ptr;
}

/*
 * 关闭文件，减少引用计数
 * 当引用计数归零时释放inode
 */
void file_close(struct File *file_ptr) {
    struct File file_copy;
    
    if(file_ptr->ref < 1) {
        panic("file_close: 文件引用计数无效");
    }
    
    // 减少引用计数
    if(--file_ptr->ref > 0) {
        return;  // 还有其他引用，不释放
    }
    
    // 保存副本，因为要清空原结构
    file_copy = *file_ptr;
    file_ptr->ref = 0;
    file_ptr->type = FILE_NONE;
    
    // 释放inode
    if(file_copy.type == FILE_INODE) {
        begin_op();
        iput(file_copy.ip);
        end_op();
    }
}

/*
 * 获取文件状态
 * 简化版本：暂不完整实现
 */
int file_stat(struct File *file_ptr, uint64 addr) {
    // 简化实现，实际应该填充stat结构
    return 0;
}

/*
 * 从文件读取数据
 */
int file_read(struct File *file_ptr, uint64 user_addr, int byte_count) {
    int bytes_read = 0;
    
    // 检查读权限
    if(file_ptr->readable == 0) {
        return -1;
    }
    
    if(file_ptr->type == FILE_INODE) {
        ilock(file_ptr->ip);
        bytes_read = readi(file_ptr->ip, 1, user_addr, file_ptr->off, byte_count);
        
        // 读取成功，更新文件偏移
        if(bytes_read > 0) {
            file_ptr->off += bytes_read;
        }
        
        iunlock(file_ptr->ip);
        return bytes_read;
    }
    
    panic("file_read: 不支持的文件类型");
    return -1;
}

/*
 * 向文件写入数据
 * 支持追加模式和普通写入模式
 */
int file_write(struct File *file_ptr, uint64 user_addr, int byte_count) {
    int bytes_written, total_written = 0;
    
    // 检查写权限
    if(file_ptr->writable == 0) {
        return -1;
    }
    
    if(file_ptr->type == FILE_INODE) {
        // 计算单次写入的最大字节数
        // 每个事务最多可以修改的块数：
        // (LOGSIZE - 1(日志头) - 1(inode) - 2(间接块+位图)) / 2 * BSIZE
        int max_write_per_tx = ((LOGSIZE - 1 - 1 - 2) / 2) * BSIZE;
        int written_offset = 0;
        
        // 分批写入，避免单个事务过大
        while(written_offset < byte_count) {
            int chunk_size = byte_count - written_offset;
            if(chunk_size > max_write_per_tx) {
                chunk_size = max_write_per_tx;
            }
            
            begin_op();
            ilock(file_ptr->ip);
            
            // 追加模式：每次写入前都移到文件末尾
            if(file_ptr->append) {
                file_ptr->off = file_ptr->ip->size;
            }
            
            // 执行写入
            bytes_written = writei(file_ptr->ip, 1, 
                                   user_addr + written_offset, 
                                   file_ptr->off, 
                                   chunk_size);
            
            // 更新文件偏移
            if(bytes_written > 0) {
                file_ptr->off += bytes_written;
            }
            
            iunlock(file_ptr->ip);
            end_op();
            
            // 如果写入失败，停止
            if(bytes_written != chunk_size) {
                break;
            }
            
            written_offset += bytes_written;
        }
        
        // 返回总写入字节数，如果全部成功则返回byte_count，否则返回-1
        total_written = (written_offset == byte_count) ? byte_count : -1;
        return total_written;
    }
    
    panic("file_write: 不支持的文件类型");
    return -1;
}