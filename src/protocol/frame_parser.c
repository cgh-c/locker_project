#include "frame_parser.h"
#include <string.h>

/* 帧头 "WKLY" */
static const uint8_t FRAME_HEADER[4] = {0x57, 0x4B, 0x4C, 0x59};

/* 内部状态 */
enum {
    ST_HEADER = 0,   /* 正在匹配帧头（进度由 pos 表达: 0~3） */
    ST_LEN,          /* 等待长度字节 */
    ST_BODY,         /* 收帧体直到 frame_len 字节收满 */
};

void frame_parser_init(frame_parser_t *p)
{
    p->state     = ST_HEADER;
    p->pos       = 0;
    p->frame_len = 0;
}

/*
 * 重同步：当前字节不匹配预期时调用。
 * 关键点：失配字节本身可能是下一个帧头的开始（例: 57 57 4B 4C 59），
 * 简单地 pos=0 丢弃会漏掉真帧。帧头 57 4B 4C 59 没有长度>1的自重复
 * 前缀，所以只需检查失配字节是否等于首字节 0x57。
 */
static void resync(frame_parser_t *p, uint8_t byte)
{
    p->state = ST_HEADER;
    if (byte == FRAME_HEADER[0]) {
        p->buf[0] = byte;
        p->pos = 1;
    } else {
        p->pos = 0;
    }
}

static uint8_t compute_xor(const uint8_t *data, int len)
{
    uint8_t x = 0;
    for (int i = 0; i < len; i++) {
        x ^= data[i];
    }
    return x;
}

int frame_parser_feed(frame_parser_t *p, uint8_t byte)
{
    switch (p->state) {

    case ST_HEADER:
        if (byte == FRAME_HEADER[p->pos]) {
            p->buf[p->pos++] = byte;
            if (p->pos == 4) {
                p->state = ST_LEN;
            }
        } else {
            resync(p, byte);
        }
        return FRAME_FEED_IDLE;

    case ST_LEN:
        if (byte >= FRAME_MIN_LEN) {   /* uint8_t 上限 0xFF 天然满足 */
            p->buf[4]    = byte;
            p->frame_len = byte;
            p->pos       = 5;
            p->state     = ST_BODY;
            return FRAME_FEED_IDLE;
        }
        /* 长度非法：丢弃已攒帧头，该字节按重同步规则再过一遍
         * （<0x08 的字节不可能是 0x57，实际必然 pos=0，写成通用形式） */
        resync(p, byte);
        return FRAME_FEED_ERR_LEN;

    case ST_BODY:
        p->buf[p->pos++] = byte;
        if (p->pos < p->frame_len) {
            return FRAME_FEED_IDLE;
        }
        /* 收满一帧：验 XOR（除校验字节外全部字节异或） */
        p->state = ST_HEADER;
        p->pos   = 0;
        if (compute_xor(p->buf, p->frame_len - 1) == p->buf[p->frame_len - 1]) {
            return FRAME_FEED_COMPLETE;   /* buf/frame_len 供调用方取用 */
        }
        return FRAME_FEED_ERR_XOR;

    default:                              /* 不可达，防御性复位 */
        frame_parser_init(p);
        return FRAME_FEED_IDLE;
    }
}
