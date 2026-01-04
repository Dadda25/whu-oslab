#ifndef _LOG_H_
#define _LOG_H_

#include "common.h"
#include "bio.h"

// 日志系统函数
void log_init(int dev, struct superblock *sb);
void begin_op(void);
void end_op(void);
void log_write(struct buf *b);

#endif // _LOG_H_
