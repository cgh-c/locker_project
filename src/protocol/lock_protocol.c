#include "lock_protocol.h"
#include <string.h>

static const uint8_t FRAME_HEADER[4] = {0x57, 0x4B, 0x4C, 0x59};

static uint8_t compute_xor(const uint8_t *data, uint16_t len)
{
    uint8_t xor_sum = 0;
    for (uint16_t i = 0; i < len; i++) {
        xor_sum ^= data[i];
    }
    return xor_sum;
}

/* 内部：拼通用帧，data 是指令码之后的可变数据域（开全部时 data_len=0） */
static int build_frame(uint8_t addr, uint8_t cmd,
                       const uint8_t *data, int data_len,
                       uint8_t *buf, int buf_size)
{
    int frame_len = 4 + 1 + 1 + 1 + data_len + 1;  /* 头+长+址+令+数据+校验 */
    if (buf == NULL || buf_size < frame_len) return PROTO_ERR_PARAM;
    memcpy(buf, FRAME_HEADER, 4);
    buf[4] = (uint8_t)frame_len;
    buf[5] = addr;
    buf[6] = cmd;
    if (data_len > 0) memcpy(&buf[7], data, data_len);
    buf[frame_len - 1] = compute_xor(buf, frame_len - 1);
    return frame_len;
}

int proto_build_open_single(uint8_t addr, uint8_t box_id, uint8_t *buf, int buf_size)
{
    if (box_id < 1) return PROTO_ERR_PARAM;   /* 上限由上层用 box_count 检查 */
    return build_frame(addr, CMD_OPEN_SINGLE, &box_id, 1, buf, buf_size);
}

int proto_build_read_single(uint8_t addr, uint8_t box_id, uint8_t *buf, int buf_size)
{
    if (box_id < 1) return PROTO_ERR_PARAM;   /* 上限由上层用 box_count 检查 */
    return build_frame(addr, CMD_READ_SINGLE, &box_id, 1, buf, buf_size);
}

int proto_build_open_all(uint8_t addr, uint8_t *buf, int buf_size)
{
    return build_frame(addr, CMD_OPEN_ALL, NULL, 0, buf, buf_size);
}

int proto_build_read_all(uint8_t addr, uint8_t *buf, int buf_size)
{
    return build_frame(addr, CMD_READ_ALL, NULL, 0, buf, buf_size);
}

int proto_parse_reply(const uint8_t *buf, int len, uint8_t expect_cmd, lock_reply_t *reply)
{
    if (!buf || !reply || len < PROTO_RSP_LEN_ALL)      return PROTO_ERR_PARAM;
    if (memcmp(buf, FRAME_HEADER, 4) != 0)              return PROTO_ERR_HEADER;
    if (buf[4] != len)                                  return PROTO_ERR_LENGTH;
    if (compute_xor(buf, len - 1) != buf[len - 1])      return PROTO_ERR_XOR;     /* 新增！ */
    if (buf[6] != expect_cmd)                           return PROTO_ERR_CMD;

    reply->addr      = buf[5];
    reply->cmd       = buf[6];
    reply->op_status = buf[7];
    reply->channel     = (len >= PROTO_RSP_LEN_SINGLE) ? buf[8] : 0;
    reply->lock_status = (len >= PROTO_RSP_LEN_SINGLE) ? buf[9] : 0;
    return PROTO_OK;
}

int proto_parse_read_all(const uint8_t *buf, int len,
                         uint8_t *statuses, int max_count, int *count)
{
    if (!buf || !statuses || !count || len < PROTO_RSP_LEN_READ_ALL(1))
        return PROTO_ERR_PARAM;
    if (memcmp(buf, FRAME_HEADER, 4) != 0)              return PROTO_ERR_HEADER;
    if (buf[4] != len)                                  return PROTO_ERR_LENGTH;
    if (compute_xor(buf, len - 1) != buf[len - 1])      return PROTO_ERR_XOR;
    if (buf[6] != CMD_READ_ALL)                         return PROTO_ERR_CMD;

    int n = buf[8];                       /* 锁数量字段（偏移8），实测 0x0C=12 */
    if (PROTO_RSP_LEN_READ_ALL(n) != len) return PROTO_ERR_LENGTH;  /* 数量与帧长自洽性 */
    if (n > max_count)                    return PROTO_ERR_PARAM;

    memcpy(statuses, &buf[9], n);         /* 偏移9起 n 个锁状态字节 */
    *count = n;
    return PROTO_OK;
}
