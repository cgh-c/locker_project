#ifndef _FRAME_PARSER_H
#define _FRAME_PARSER_H

#include <stdint.h>

/*
 * 帧同步状态机（协议层，纯逻辑，零 I/O 依赖）
 *
 * 职责：把无边界的串口字节流还原成完整的协议帧。
 *   - 自动跳过垃圾字节、错位数据（重同步）
 *   - 按帧内长度字节（偏移4）确定帧边界，通吃所有指令（含 0x85 主动上报）
 *   - 完成 XOR 校验，坏帧丢弃后自动恢复
 *
 * 不管超时：半包卡住（如收到帧头后线断）由调用方在命令超时后
 * 调 frame_parser_init() 复位。
 *
 * 已知限制：XOR 校验失败时丢弃整帧缓冲、从下一字节重新找头，
 * 不回溯扫描被丢弃字节内部可能藏着的真帧头。触发前提是总线碰撞
 * 恰好伪造出合法帧头+合法长度，概率可忽略；上层有命令重试兜底。
 */

#define FRAME_MAX_LEN   255   /* 协议帧长字段上限 0xFF */
#define FRAME_MIN_LEN   8     /* 协议规定帧长最小 0x08 */

/* frame_parser_feed() 的返回值 */
#define FRAME_FEED_IDLE      0   /* 还在攒字节，无事发生 */
#define FRAME_FEED_COMPLETE  1   /* 一个完整帧就绪（已通过XOR校验） */
#define FRAME_FEED_ERR_XOR  -1   /* 收满一帧但校验失败，已丢弃并重同步 */
#define FRAME_FEED_ERR_LEN  -2   /* 长度字节非法(<0x08)，已丢弃并重同步 */

typedef struct {
    int     state;                  /* 内部状态，调用方勿动 */
    uint8_t buf[FRAME_MAX_LEN];     /* 组帧缓冲 */
    int     pos;                    /* 已收字节数 */
    int     frame_len;              /* 本帧声明的总长度 */
} frame_parser_t;

/**
 * @brief 初始化/复位状态机（丢弃攒了一半的数据）
 */
void frame_parser_init(frame_parser_t *p);

/**
 * @brief 喂入一个字节
 * @return FRAME_FEED_COMPLETE 时，完整帧在 p->buf 中、长度为 p->frame_len，
 *         内容在下一次 feed 之前保持有效，调用方须及时取走；
 *         其余返回值见宏定义。
 */
int frame_parser_feed(frame_parser_t *p, uint8_t byte);

#endif /* _FRAME_PARSER_H */
