/*
 * VirtIO 块设备驱动
 * 通过MMIO接口与QEMU的virtio磁盘设备通信
 * 
 * QEMU启动参数示例：
 * qemu ... -drive file=fs.img,if=none,format=raw,id=x0 
 *          -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0
 */

#include "memlayout.h"
#include "common.h"
#include "riscv.h"
#include "fs/fs.h"
#include "fs/bio.h"
#include "fs/file.h"
#include "mem/pmem.h"
#include "dev/virtio.h"
#include "lib/print.h"
#include "lib/str.h"

// MMIO寄存器访问宏
// 注意：VIRTIO0 定义在 memlayout.h 中为 0x10001000
#define MMIO_REG(offset) ((volatile uint32 *)(VIRTIO0 + (offset)))

// VirtIO队列对齐要求
#define QUEUE_ALIGN 16

// 描述符数量（必须是2的幂）
#define DESC_COUNT 8

// VirtIO磁盘管理结构
typedef struct {
    struct virtq_desc *descriptors;     // 描述符表
    struct virtq_avail *avail_ring;     // 可用环
    struct virtq_used *used_ring;       // 已用环
    
    char desc_free[DESC_COUNT];         // 描述符空闲标记
    uint16 used_index;                  // 已处理的used索引
    
    // 每个描述符的请求信息
    struct {
        struct buf *buffer;             // 关联的缓冲区
        char completion_status;         // 完成状态
    } request_info[DESC_COUNT];
    
    struct virtio_blk_req operations[DESC_COUNT];  // 请求头
} virtio_disk_t;

static virtio_disk_t vdisk;

// 内部函数声明
static int descriptor_alloc(void);
static void descriptor_free(int index);
static void descriptor_chain_free(int head);
static int allocate_three_descriptors(int *indices);
static void process_completed_requests(void);

/*
 * 初始化VirtIO磁盘设备
 */
void virtio_disk_init(void) {
    uint32 device_status = 0;
    
    // 验证设备
    uint32 magic_value = *MMIO_REG(VIRTIO_MMIO_MAGIC_VALUE);
    uint32 version = *MMIO_REG(VIRTIO_MMIO_VERSION);
    uint32 device_id = *MMIO_REG(VIRTIO_MMIO_DEVICE_ID);
    uint32 vendor_id = *MMIO_REG(VIRTIO_MMIO_VENDOR_ID);
    
    if(magic_value != 0x74726976 || 
       (version != 1 && version != 2) || 
       device_id != 2 || 
       vendor_id != 0x554d4551) {
        printf("virtio_disk_init: 设备探测失败\n");
        printf("  magic=0x%x version=0x%x device=0x%x vendor=0x%x\n",
               magic_value, version, device_id, vendor_id);
        panic("virtio_disk_init: 未找到有效的virtio磁盘设备");
    }
    
    // 设备初始化步骤
    *MMIO_REG(VIRTIO_MMIO_STATUS) = device_status;
    
    // 1. 确认设备存在
    device_status |= VIRTIO_CONFIG_S_ACKNOWLEDGE;
    *MMIO_REG(VIRTIO_MMIO_STATUS) = device_status;
    
    // 2. 声明我们知道如何驱动此设备
    device_status |= VIRTIO_CONFIG_S_DRIVER;
    *MMIO_REG(VIRTIO_MMIO_STATUS) = device_status;
    
    // 3. 协商特性
    uint64 device_features = *MMIO_REG(VIRTIO_MMIO_DEVICE_FEATURES);
    
    // 禁用我们不需要的特性
    device_features &= ~(1ULL << VIRTIO_BLK_F_RO);
    device_features &= ~(1ULL << VIRTIO_BLK_F_SCSI);
    device_features &= ~(1ULL << VIRTIO_BLK_F_CONFIG_WCE);
    device_features &= ~(1ULL << VIRTIO_BLK_F_MQ);
    device_features &= ~(1ULL << VIRTIO_F_ANY_LAYOUT);
    device_features &= ~(1ULL << VIRTIO_RING_F_EVENT_IDX);
    device_features &= ~(1ULL << VIRTIO_RING_F_INDIRECT_DESC);
    
    *MMIO_REG(VIRTIO_MMIO_DRIVER_FEATURES) = device_features;
    
    // 4. 特性协商完成
    device_status |= VIRTIO_CONFIG_S_FEATURES_OK;
    *MMIO_REG(VIRTIO_MMIO_STATUS) = device_status;
    
    // 5. 验证特性OK位
    device_status = *MMIO_REG(VIRTIO_MMIO_STATUS);
    if((device_status & VIRTIO_CONFIG_S_FEATURES_OK) == 0) {
        panic("virtio_disk_init: 设备不接受我们的特性");
    }
    
    // 6. 配置队列
    *MMIO_REG(VIRTIO_MMIO_QUEUE_SEL) = 0;
    
    if(*MMIO_REG(VIRTIO_MMIO_QUEUE_READY)) {
        panic("virtio_disk_init: 队列已就绪，异常状态");
    }
    
    uint32 max_queue_size = *MMIO_REG(VIRTIO_MMIO_QUEUE_NUM_MAX);
    if(max_queue_size == 0) {
        panic("virtio_disk_init: 队列0不存在");
    }
    if(max_queue_size < DESC_COUNT) {
        panic("virtio_disk_init: 队列大小不足");
    }
    
    // 分配队列内存（一个页面）
    char *queue_memory = (char *)pmem_alloc(true);
    if(queue_memory == 0) {
        panic("virtio_disk_init: 内存分配失败");
    }
    
    memset((addr_t)queue_memory, 0, PGSIZE);
    
    // 设置队列布局
    vdisk.descriptors = (struct virtq_desc *)queue_memory;
    vdisk.avail_ring = (struct virtq_avail *)(queue_memory + 
                                               DESC_COUNT * sizeof(struct virtq_desc));
    
    // 计算used ring的对齐地址
    char *avail_end = (char *)vdisk.avail_ring + sizeof(struct virtq_avail);
    char *used_aligned = (char *)(((uint64)avail_end + QUEUE_ALIGN - 1) & 
                                  ~(uint64)(QUEUE_ALIGN - 1));
    vdisk.used_ring = (struct virtq_used *)used_aligned;
    
    // 验证布局适合一个页面
    if(used_aligned + sizeof(struct virtq_used) > queue_memory + PGSIZE) {
        panic("virtio_disk_init: virtio ring太大");
    }
    
    // 配置MMIO寄存器
    *MMIO_REG(VIRTIO_MMIO_GUEST_PAGE_SIZE) = PGSIZE;
    *MMIO_REG(VIRTIO_MMIO_QUEUE_ALIGN) = QUEUE_ALIGN;
    *MMIO_REG(VIRTIO_MMIO_QUEUE_NUM) = DESC_COUNT;
    *MMIO_REG(VIRTIO_MMIO_QUEUE_PFN) = ((uint64)queue_memory) >> 12;
    *MMIO_REG(VIRTIO_MMIO_QUEUE_READY) = 0x1;
    
    // 初始化描述符空闲列表
    for(int i = 0; i < DESC_COUNT; i++) {
        vdisk.desc_free[i] = 1;
    }
    
    // 7. 设备准备就绪
    device_status |= VIRTIO_CONFIG_S_DRIVER_OK;
    *MMIO_REG(VIRTIO_MMIO_STATUS) = device_status;
}

/*
 * 分配一个空闲描述符
 */
static int descriptor_alloc(void) {
    for(int i = 0; i < DESC_COUNT; i++) {
        if(vdisk.desc_free[i]) {
            vdisk.desc_free[i] = 0;
            return i;
        }
    }
    return -1;
}

/*
 * 释放一个描述符
 */
static void descriptor_free(int index) {
    if(index >= DESC_COUNT) {
        panic("descriptor_free: 索引越界");
    }
    if(vdisk.desc_free[index]) {
        panic("descriptor_free: 重复释放");
    }
    
    // 清空描述符
    vdisk.descriptors[index].addr = 0;
    vdisk.descriptors[index].len = 0;
    vdisk.descriptors[index].flags = 0;
    vdisk.descriptors[index].next = 0;
    
    vdisk.desc_free[index] = 1;
}

/*
 * 释放描述符链
 */
static void descriptor_chain_free(int head) {
    while(1) {
        int has_next = vdisk.descriptors[head].flags & VRING_DESC_F_NEXT;
        int next_index = vdisk.descriptors[head].next;
        
        descriptor_free(head);
        
        if(has_next) {
            head = next_index;
        } else {
            break;
        }
    }
}

/*
 * 分配三个描述符（用于一个完整的磁盘请求）
 */
static int allocate_three_descriptors(int *indices) {
    for(int i = 0; i < 3; i++) {
        indices[i] = descriptor_alloc();
        if(indices[i] < 0) {
            // 分配失败，释放已分配的
            for(int j = 0; j < i; j++) {
                descriptor_free(indices[j]);
            }
            return -1;
        }
    }
    return 0;
}

/*
 * 处理已完成的请求
 */
static void process_completed_requests(void) {
    __sync_synchronize();
    
    while(vdisk.used_index != vdisk.used_ring->idx) {
        __sync_synchronize();
        
        int request_id = vdisk.used_ring->ring[vdisk.used_index % DESC_COUNT].id;
        
        // 检查完成状态
        if(vdisk.request_info[request_id].completion_status != 0) {
            panic("virtio_disk: 请求完成状态异常");
        }
        
        // 标记缓冲区完成
        struct buf *completed_buffer = vdisk.request_info[request_id].buffer;
        if(completed_buffer) {
            completed_buffer->disk = 0;
        }
        
        vdisk.used_index += 1;
    }
}

/*
 * 执行磁盘读写操作
 * is_write: 1表示写，0表示读
 */
void virtio_disk_rw(struct buf *buffer, int is_write) {
    uint64 sector_num = buffer->blockno * (BSIZE / 512);
    
    int desc_indices[3];
    
    // 等待可用描述符
    while(allocate_three_descriptors(desc_indices) != 0) {
        process_completed_requests();
    }
    
    // 准备请求头
    struct virtio_blk_req *request_header = &vdisk.operations[desc_indices[0]];
    request_header->type = is_write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
    request_header->reserved = 0;
    request_header->sector = sector_num;
    
    // 描述符0：请求头
    vdisk.descriptors[desc_indices[0]].addr = (uint64)request_header;
    vdisk.descriptors[desc_indices[0]].len = sizeof(struct virtio_blk_req);
    vdisk.descriptors[desc_indices[0]].flags = VRING_DESC_F_NEXT;
    vdisk.descriptors[desc_indices[0]].next = desc_indices[1];
    
    // 描述符1：数据缓冲区
    vdisk.descriptors[desc_indices[1]].addr = (uint64)buffer->data;
    vdisk.descriptors[desc_indices[1]].len = BSIZE;
    vdisk.descriptors[desc_indices[1]].flags = is_write ? 0 : VRING_DESC_F_WRITE;
    vdisk.descriptors[desc_indices[1]].flags |= VRING_DESC_F_NEXT;
    vdisk.descriptors[desc_indices[1]].next = desc_indices[2];
    
    // 描述符2：状态字节
    vdisk.request_info[desc_indices[0]].completion_status = 0xff;
    vdisk.descriptors[desc_indices[2]].addr = 
        (uint64)&vdisk.request_info[desc_indices[0]].completion_status;
    vdisk.descriptors[desc_indices[2]].len = 1;
    vdisk.descriptors[desc_indices[2]].flags = VRING_DESC_F_WRITE;
    vdisk.descriptors[desc_indices[2]].next = 0;
    
    // 记录请求信息
    buffer->disk = 1;
    vdisk.request_info[desc_indices[0]].buffer = buffer;
    
    // 将请求添加到可用环
    vdisk.avail_ring->ring[vdisk.avail_ring->idx % DESC_COUNT] = desc_indices[0];
    __sync_synchronize();
    vdisk.avail_ring->idx += 1;
    __sync_synchronize();
    
    // 通知设备
    *MMIO_REG(VIRTIO_MMIO_QUEUE_NOTIFY) = 0;
    
    // 等待完成
    while(buffer->disk == 1) {
        process_completed_requests();
    }
    
    // 清理请求
    vdisk.request_info[desc_indices[0]].buffer = 0;
    descriptor_chain_free(desc_indices[0]);
}

/*
 * VirtIO磁盘中断处理
 */
void virtio_disk_intr(void) {
    uint32 interrupt_status = *MMIO_REG(VIRTIO_MMIO_INTERRUPT_STATUS) & 0x3;
    
    if(interrupt_status) {
        *MMIO_REG(VIRTIO_MMIO_INTERRUPT_ACK) = interrupt_status;
    }
    
    process_completed_requests();
}