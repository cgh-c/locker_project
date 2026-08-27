#ifndef _LOCK_PROTOCOL_H
#define _LOCK_PROTOCOL_H

#include <stdint.h>

/* 指令码 */
#define CMD_OPEN_SINGLE      0x82
#define CMD_READ_SINGLE      0x83
#define CMD_READ_ALL         0x84
#define CMD_OPEN_ALL         0x86

/* 锁状态 / 执行状态 */
#define LOCK_STATUS_CLOSED   0x00
#define LOCK_STATUS_OPEN     0x01
#define LOCK_OP_SUCCESS      0x00

/* 各指令的帧长（组帧函数返回值 / 调用方分配缓冲区的依据） */
#define PROTO_REQ_LEN_SINGLE   9   /* 开单锁/读单锁请求 */
#define PROTO_REQ_LEN_ALL      8   /* 开全部/读全部请求 */
#define PROTO_RSP_LEN_SINGLE  11   /* 单锁指令回复 */
#define PROTO_RSP_LEN_ALL      9   /* 开全部回复 */
/* 0x84 读全部回复帧长 = 10 + 锁数量
 * （头4+长1+址1+令1+状态1+数量1+校验1 = 10 固定字节；12路板实测 22 字节） */
#define PROTO_RSP_LEN_READ_ALL(n)  (10 + (n))

/* 解析结果：一次性解出回复帧里所有有意义的字段 */
typedef struct {
    uint8_t addr;         /* 板地址 */
    uint8_t cmd;          /* 指令码回显 */
    uint8_t op_status;    /* 执行状态: 0=成功 */
    uint8_t channel;      /* 通道号（仅单锁回复有效） */
    uint8_t lock_status;  /* 锁状态（仅读锁回复有效: 0关/1开） */
} lock_reply_t;

/* 错误码（协议层自己的，不和 HAL 层的混用） */
#define PROTO_OK           0
#define PROTO_ERR_PARAM   -1   /* 参数非法/缓冲区太小 */
#define PROTO_ERR_HEADER  -2   /* 帧头不对 */
#define PROTO_ERR_LENGTH  -3   /* 长度字段与实际不符 */
#define PROTO_ERR_CMD     -4   /* 指令码不匹配 */
#define PROTO_ERR_XOR     -5   /* 校验错误 */

/* 组帧：成功返回帧长度(>0)，失败返回 PROTO_ERR_PARAM */
int proto_build_open_single(uint8_t addr, uint8_t box_id, uint8_t *buf, int buf_size);
int proto_build_read_single(uint8_t addr, uint8_t box_id, uint8_t *buf, int buf_size);
int proto_build_open_all(uint8_t addr, uint8_t *buf, int buf_size);
int proto_build_read_all(uint8_t addr, uint8_t *buf, int buf_size);

/* 解帧：成功返回 PROTO_OK 并填充 reply，失败返回负错误码 */
int proto_parse_reply(const uint8_t *buf, int len, uint8_t expect_cmd, lock_reply_t *reply);

/**
 * @brief 解析 0x84"读取所有锁状态"回复（变长: 9+锁数量 字节）
 * @param buf       完整回复帧
 * @param len       帧总长
 * @param statuses  输出数组，每个元素一个锁: 0关/1开
 * @param max_count 数组容量
 * @param count     输出实际锁数量（帧内偏移8的锁数量字段）
 * @return PROTO_OK 或负错误码
 */
int proto_parse_read_all(const uint8_t *buf, int len,
                         uint8_t *statuses, int max_count, int *count);

#endif