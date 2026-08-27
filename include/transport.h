#ifndef _TRANSPORT_H
#define _TRANSPORT_H

#include <stdint.h>

/**
 * @brief 帧接收回调：当状态机收完一个完整帧时调用
 * @param buf  完整帧数据（含帧头、长度、载荷、校验）
 * @param len  帧总长度
 * @param user_arg 用户自定义指针（可指向 Core 层实例）
 */
typedef void (*transport_on_frame_cb)(uint8_t *buf, int len, void *user_arg);

/**
 * @brief 初始化传输层
 * @param fd        已打开的串口文件描述符
 * @param cb        帧接收回调
 * @param user_arg  回调时透传的用户参数
 * @return 0成功，-1失败
 */
int transport_init(int fd, transport_on_frame_cb cb, void *user_arg);

/**
 * @brief 发送数据（异步，无等待）
 * @param data 待发送字节数组（已由 lock_protocol 组好帧）
 * @param len  数据长度
 * @return 0成功，-1失败
 */
int transport_send(const uint8_t *data, int len);

/**
 * @brief 停止传输层（销毁线程，清理资源）
 */
void transport_deinit(void);

#endif // _TRANSPORT_H