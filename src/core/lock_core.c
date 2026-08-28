#include "lock_core.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>

#include "lock_protocol.h"
#include "frame_parser.h"
#include "transport.h"
#include "hal_locker.h"

/*
 * 命令收发核心层实现
 *
 * 工作方式：
 *   1. lock_core_init 时启动 transport 接收线程，注册 on_frame 回调。
 *   2. transport 线程持续读串口 → 帧状态机还原完整帧 → 调 on_frame。
 *   3. on_frame 在接收线程里执行，按帧里的命令码分流：
 *        - 0x85 事件帧：直接调上层注册的事件回调（立即上抛，不落地）
 *        - 命令回复帧：与当前等待的命令码匹配时，拷贝帧并唤醒等待者
 *   4. 命令接口组帧后调 transport_send 发送，再在条件变量上等匹配回复。
 *
 * 这样彻底消除了「自己 read 固定字节」的旧做法：
 *   - 0x85 事件被实时消费，不再堆积污染串口缓冲区
 *   - 命令回复按命令码精确匹配，不依赖固定长度
 */

static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cond  = PTHREAD_COND_INITIALIZER;

static uint8_t g_expect_cmd = 0;   /* 当前等待的命令码；0 表示无等待 */

static uint8_t g_rx_buf[FRAME_MAX_LEN];   /* 命令回复暂存区 */
static int     g_rx_len = 0;
static int     g_rx_ready = 0;

static lock_core_event_cb g_event_cb = NULL;   /* 0x85 事件回调 */
static void *g_event_user = NULL;

/*
 * transport 帧回调（在接收线程上下文中执行）
 * 注意：这里要快速返回，不能阻塞，否则会影响后续帧接收。
 */
static void on_frame(uint8_t *buf, int len, void *user)
{
    (void)user;

    uint8_t cmd = buf[6];   /* 命令码固定在第 6 字节（偏移 6） */

    if (cmd == CMD_EVENT_CHANGE) {
        /* 0x85 主动上报：通道号在 buf[7]，锁状态在 buf[8] */
        if (g_event_cb) {
            g_event_cb(buf[7], buf[8], g_event_user);
        }
        return;
    }

    /* 命令回复：只处理与当前等待命令码匹配的帧 */
    pthread_mutex_lock(&g_mutex);
    if (cmd == g_expect_cmd && !g_rx_ready) {
        memcpy(g_rx_buf, buf, len);
        g_rx_len = len;
        g_rx_ready = 1;
        pthread_cond_signal(&g_cond);
    }
    pthread_mutex_unlock(&g_mutex);
}

int lock_core_init(int fd, lock_core_event_cb event_cb, void *user)
{
    if (fd < 0) return -1;

    g_event_cb = event_cb;
    g_event_user = user;
    g_expect_cmd = 0;
    g_rx_ready = 0;

    return transport_init(fd, on_frame, NULL);
}

void lock_core_deinit(void)
{
    transport_deinit();
    g_event_cb = NULL;
    g_event_user = NULL;
}

/*
 * 发送命令并等待匹配回复。
 * 返回 0=成功, -1=超时, -2=发送失败
 */
static int send_and_wait(uint8_t cmd, const uint8_t *tx, int tx_len,
                         uint8_t *reply_out, int reply_cap, int *reply_len,
                         int timeout_ms)
{
    struct timespec deadline;

    pthread_mutex_lock(&g_mutex);
    g_expect_cmd = cmd;
    g_rx_ready = 0;
    g_rx_len = 0;
    pthread_mutex_unlock(&g_mutex);

    if (transport_send(tx, tx_len) < 0) {
        return -2;
    }

    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec += 1;
        deadline.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&g_mutex);
    while (!g_rx_ready) {
        int rc = pthread_cond_timedwait(&g_cond, &g_mutex, &deadline);
        if (rc == ETIMEDOUT) {
            g_expect_cmd = 0;
            pthread_mutex_unlock(&g_mutex);
            return -1;
        }
    }
    if (g_rx_len > reply_cap) {
        g_expect_cmd = 0;
        g_rx_ready = 0;
        pthread_mutex_unlock(&g_mutex);
        return -2;
    }
    memcpy(reply_out, g_rx_buf, g_rx_len);
    *reply_len = g_rx_len;
    g_expect_cmd = 0;
    g_rx_ready = 0;
    pthread_mutex_unlock(&g_mutex);

    return 0;
}

int lock_core_open_single(uint8_t box_id, int timeout_ms)
{
    uint8_t tx[PROTO_REQ_LEN_SINGLE];
    uint8_t rx[PROTO_RSP_LEN_SINGLE];
    int rx_len = 0;
    lock_reply_t rp;

    if (box_id < 1 || box_id > 12) return -2;
    if (timeout_ms <= 0) timeout_ms = LOCK_DEFAULT_TIMEOUT_MS;

    int len = proto_build_open_single(DEFAULT_BOARD_ADDR, box_id, tx, sizeof(tx));
    if (len < 0) return -2;

    int ret = send_and_wait(CMD_OPEN_SINGLE, tx, len, rx, sizeof(rx), &rx_len, timeout_ms);
    if (ret < 0) return ret;

    if (proto_parse_reply(rx, rx_len, CMD_OPEN_SINGLE, &rp) != PROTO_OK)
        return -2;
    if (rp.op_status != LOCK_OP_SUCCESS) return -2;

    return 0;
}

int lock_core_read_single(uint8_t box_id, uint8_t *status, int timeout_ms)
{
    uint8_t tx[PROTO_REQ_LEN_SINGLE];
    uint8_t rx[PROTO_RSP_LEN_SINGLE];
    int rx_len = 0;
    lock_reply_t rp;

    if (box_id < 1 || box_id > 12 || status == NULL) return -2;
    if (timeout_ms <= 0) timeout_ms = LOCK_DEFAULT_TIMEOUT_MS;

    int len = proto_build_read_single(DEFAULT_BOARD_ADDR, box_id, tx, sizeof(tx));
    if (len < 0) return -2;

    int ret = send_and_wait(CMD_READ_SINGLE, tx, len, rx, sizeof(rx), &rx_len, timeout_ms);
    if (ret < 0) return ret;

    if (proto_parse_reply(rx, rx_len, CMD_READ_SINGLE, &rp) != PROTO_OK)
        return -2;
    if (rp.op_status != LOCK_OP_SUCCESS) return -2;

    *status = rp.lock_status;
    return 0;
}

int lock_core_open_all(int timeout_ms)
{
    uint8_t tx[PROTO_REQ_LEN_ALL];
    uint8_t rx[PROTO_RSP_LEN_ALL];
    int rx_len = 0;
    lock_reply_t rp;

    if (timeout_ms <= 0) timeout_ms = LOCK_DEFAULT_TIMEOUT_MS;

    int len = proto_build_open_all(DEFAULT_BOARD_ADDR, tx, sizeof(tx));
    if (len < 0) return -2;

    int ret = send_and_wait(CMD_OPEN_ALL, tx, len, rx, sizeof(rx), &rx_len, timeout_ms);
    if (ret < 0) return ret;

    if (proto_parse_reply(rx, rx_len, CMD_OPEN_ALL, &rp) != PROTO_OK)
        return -2;
    if (rp.op_status != LOCK_OP_SUCCESS) return -2;

    return 0;
}

int lock_core_read_all(uint8_t *statuses, int max_count, int *count, int timeout_ms)
{
    uint8_t tx[PROTO_REQ_LEN_ALL];
    /* 0x84 回复帧长 = 10 + 锁数量，12 路板最大 22 字节 */
    uint8_t rx[PROTO_RSP_LEN_READ_ALL(12)];
    int rx_len = 0;

    if (statuses == NULL || count == NULL) return -2;
    if (timeout_ms <= 0) timeout_ms = LOCK_DEFAULT_TIMEOUT_MS;

    int len = proto_build_read_all(DEFAULT_BOARD_ADDR, tx, sizeof(tx));
    if (len < 0) return -2;

    int ret = send_and_wait(CMD_READ_ALL, tx, len, rx, sizeof(rx), &rx_len, timeout_ms);
    if (ret < 0) return ret;

    return proto_parse_read_all(rx, rx_len, statuses, max_count, count);
}
