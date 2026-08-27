#include "transport.h"
#include "frame_parser.h"
#include "hal_locker.h"      // 需要 hal_serial_send() 来处理 RS485 方向切换
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/select.h>

/* ---------- 静态全局变量（模块私有） ---------- */
static int g_fd = -1;                       // 串口文件描述符
static volatile int g_running = 0;          // 线程运行标志（volatile 防止编译器优化）
static pthread_t g_thread;                  // 接收线程 ID
static frame_parser_t g_parser;             // 帧同步状态机实例
static pthread_mutex_t g_tx_mutex = PTHREAD_MUTEX_INITIALIZER;  // 发送互斥锁

static transport_on_frame_cb g_frame_cb = NULL;   // 用户回调函数
static void *g_user_arg = NULL;                   // 回调透传参数

/* ---------- 接收线程主函数 ---------- */
static void *receive_thread_func(void *arg)
{
    (void)arg;  // 未使用参数

    uint8_t byte;
    fd_set read_fds;
    struct timeval tv;
    int ret;

    printf("[Transport] Receiver thread started.\n");

    while (g_running) {
        // 使用 select + 超时（100ms），以便定期检查 g_running 标志
        FD_ZERO(&read_fds);
        FD_SET(g_fd, &read_fds);
        tv.tv_sec = 0;
        tv.tv_usec = 100000;  // 100ms

        ret = select(g_fd + 1, &read_fds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR) continue;  // 被信号中断，继续
            perror("[Transport] select error");
            break;
        } else if (ret == 0) {
            // 超时（100ms 无数据），循环继续检查 g_running
            continue;
        }

        // 有数据可读，逐字节读取
        ssize_t n = read(g_fd, &byte, 1);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("[Transport] read error");
            break;
        } else if (n == 0) {
            // 串口被关闭（EOF）
            printf("[Transport] Serial port EOF, receiver exiting.\n");
            break;
        }

        // ---------- 核心：喂给帧同步状态机 ----------
        int feed_ret = frame_parser_feed(&g_parser, byte);

        if (feed_ret == FRAME_FEED_COMPLETE) {
            // 收完一帧完整且 XOR 校验通过，通过回调通知上层
            if (g_frame_cb) {
                g_frame_cb(g_parser.buf, g_parser.frame_len, g_user_arg);
            }
            // 注意：g_parser.buf 和 frame_len 在下次 feed 之前有效，
            // 回调函数应立即拷贝或处理，否则下次 feed 会覆盖。
        } else if (feed_ret == FRAME_FEED_ERR_XOR || feed_ret == FRAME_FEED_ERR_LEN) {
            // 坏帧被自动丢弃并重同步，只打印警告（调试时打开）
            // printf("[Transport] Bad frame detected (ret=%d)\n", feed_ret);
            // 上层可通过回调统计坏帧次数，实现总线健康监测
        }
        // FRAME_FEED_IDLE 则静默继续攒字节
    }

    printf("[Transport] Receiver thread stopped.\n");
    return NULL;
}

/* ---------- 对外接口实现 ---------- */

int transport_init(int fd, transport_on_frame_cb cb, void *user_arg)
{
    if (fd < 0) {
        fprintf(stderr, "[Transport] Invalid fd: %d\n", fd);
        return -1;
    }
    if (cb == NULL) {
        fprintf(stderr, "[Transport] Callback cannot be NULL\n");
        return -1;
    }

    // 如果已经初始化过，先清理旧资源
    if (g_running) {
        transport_deinit();
    }

    g_fd = fd;
    g_frame_cb = cb;
    g_user_arg = user_arg;

    // 初始化状态机（清空残留状态）
    frame_parser_init(&g_parser);

    // 启动接收线程
    g_running = 1;
    if (pthread_create(&g_thread, NULL, receive_thread_func, NULL) != 0) {
        perror("[Transport] pthread_create failed");
        g_running = 0;
        g_fd = -1;
        return -1;
    }

    printf("[Transport] Init success (fd=%d)\n", fd);
    return 0;
}

int transport_send(const uint8_t *data, int len)
{
    if (g_fd < 0 || data == NULL || len <= 0) {
        return -1;
    }

    // 加锁，防止多个线程同时调用 hal_serial_send 造成 GPIO 竞争
    pthread_mutex_lock(&g_tx_mutex);

    // 调用 HAL 层的发送函数（它内部会处理 RS485 方向切换 + tcdrain + 延时）
    int ret = hal_serial_send(g_fd, data, len);
    if (ret < 0) {
        perror("[Transport] hal_serial_send failed");
    } else {
        // 可选：打印发送的帧（调试用）
        #ifdef DEBUG_TRANSPORT
        printf("[Transport] TX (%d bytes): ", len);
        for (int i = 0; i < len; i++) printf("%02X ", data[i]);
        printf("\n");
        #endif
    }

    pthread_mutex_unlock(&g_tx_mutex);
    return ret;
}

void transport_deinit(void)
{
    if (!g_running) {
        return;
    }

    printf("[Transport] Deinitializing...\n");

    // 1. 停止接收线程
    g_running = 0;
    if (g_thread) {
        // 等待线程退出（pthread_join 会阻塞直到线程结束）
        pthread_join(g_thread, NULL);
        g_thread = 0;
    }

    // 2. 清空状态机（可选，但推荐）
    frame_parser_init(&g_parser);

    // 3. 重置全局变量（注意：不关闭 g_fd，交给调用方（main 或 HAL）去 close）
    //    因为 transport 不拥有 fd 的所有权（由 hal_serial_init 打开，main 持有）
    g_fd = -1;
    g_frame_cb = NULL;
    g_user_arg = NULL;

    printf("[Transport] Deinit done.\n");
}