#ifndef _HAL_LOCKER_H
#define _HAL_LOCKER_H

#include <stdint.h>
#include "lock_protocol.h"   /* 指令码/锁状态/帧长宏由协议层统一提供 */

#define DEFAULT_BOARD_ADDR   0x01  // 默认锁控板地址
/* 串口配置参数 */
#define LOCK_BAUDRATE        B9600   // 波特率 9600
#define LOCK_DEVICE         "/dev/ttymxc2"  // 默认设备，可传参修改

/* 超时时间（毫秒） */
#define LOCK_DEFAULT_TIMEOUT_MS  500

/**
 * @brief 初始化RS485串口
 * @param device_path 设备路径，如 "/dev/ttymxc0"，若为NULL则使用默认
 * @param baudrate    波特率，如 B9600（termios常量），若为0则用默认
 * @return 成功返回文件描述符（>=0），失败返回 -1
 */
int lock_serial_init(const char *device_path, int baudrate);

/**
 * @brief 关闭串口
 * @param fd 文件描述符
 */
void lock_serial_close(int fd);

/**
 * @brief 开单个锁
 * @param fd         串口文件描述符
 * @param box_id     通道号（1~12）
 * @param timeout_ms 等待回复超时时间（毫秒），若<=0则使用默认
 * @return 0=成功, -1=通信超时, -2=锁控板返回错误状态（非0）
 */
int hal_serial_send(int fd, const uint8_t *data, int len);
#endif // _HAL_LOCK_H

