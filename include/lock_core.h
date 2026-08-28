#ifndef _LOCK_CORE_H
#define _LOCK_CORE_H

#include <stdint.h>

/*
 * 锁命令核心层
 *
 * 这是方向 A 的关键：统一走 transport 接收线程，不再自己 read()。
 *
 * 为什么需要这一层？
 *   - 之前 hal_locker 里的 lock_open_single 等函数用 select + read 同步
 *     读固定字节，会跟 transport 的接收线程抢同一个串口 fd，并且 0x85
 *     主动上报帧（门状态变化时锁控板主动发）没人消费，堆积在缓冲区里
 *     污染后续命令回复，导致 read failed / 事件丢失。
 *   - 本层把所有串口接收交给 transport 线程 + 帧状态机，命令发出后
 *     用条件变量等待「匹配的命令码」回复；0x85 事件则通过回调上抛。
 */

/* 0x85 事件回调：channel=通道号, lock_status=0关/1开 */
typedef void (*lock_core_event_cb)(uint8_t channel, uint8_t lock_status,
                                   void *user);

/*
 * 初始化：启动 transport 接收线程，注册 0x85 事件回调。
 * fd 由调用方（locker_agent）用 lock_serial_init 打开后传入。
 * 成功返回 0，失败返回 -1。
 */
int lock_core_init(int fd, lock_core_event_cb event_cb, void *user);

/* 停止接收线程，释放资源 */
void lock_core_deinit(void);

/*
 * 下面四个命令接口都是「同步」语义：发送后阻塞等待匹配回复或超时。
 * 返回 0=成功, -1=通信超时, -2=参数/发送/解帧/执行状态错误。
 */
int lock_core_open_single(uint8_t box_id, int timeout_ms);
int lock_core_read_single(uint8_t box_id, uint8_t *status, int timeout_ms);
int lock_core_open_all(int timeout_ms);
int lock_core_read_all(uint8_t *statuses, int max_count, int *count, int timeout_ms);

#endif /* _LOCK_CORE_H */
