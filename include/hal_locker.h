#ifndef _HAL_LOCKER_H
#define _HAL_LOCKER_H

#include <stdint.h>
#define DEFAULT_BOARD_ADDR   0x01  // 默认锁控板地址
/* 串口配置参数 */
#define LOCK_BAUDRATE        B9600   // 波特率 9600
#define LOCK_DEVICE         "/dev/ttymxc2"  // 默认设备，可传参修改
/* 指令码（操作码） */
#define CMD_OPEN_SINGLE      0x82
#define CMD_READ_SINGLE      0x83
#define CMD_READ_ALL         0x84
#define CMD_OPEN_ALL         0x86

/* 锁状态定义 */
#define LOCK_STATUS_CLOSED   0x00
#define LOCK_STATUS_OPEN     0x01

/* 操作结果状态（回复帧的第一个字节） */
#define LOCK_OP_SUCCESS      0x00
#define LOCK_OP_FAIL         0x01   // 通用错误，具体可扩展

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
int lock_open_single(int fd, uint8_t box_id, int timeout_ms);

/**
 * @brief 读取单个锁状态
 * @param fd         串口文件描述符
 * @param box_id     通道号（1~12）
 * @param status     输出参数，返回锁状态（0=关闭, 1=打开）
 * @param timeout_ms 超时时间（毫秒）
 * @return 0=成功, -1=超时, -2=回复数据异常
 */
int lock_read_single(int fd, uint8_t box_id, uint8_t *status, int timeout_ms);

/**
 * @brief 打开全部锁
 * @param fd         串口文件描述符
 * @param timeout_ms 超时时间（毫秒）
 * @return 0=成功, -1=超时, -2=执行失败
 */
int lock_open_all(int fd, int timeout_ms);

#endif // _HAL_LOCK_H

