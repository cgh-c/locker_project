#ifndef _APP_CMD_H
#define _APP_CMD_H

#include <stdint.h>

#include "../mqtt/mqtt_client.h"

/*
 * 命令处理模块
 *
 * 职责：
 *   1. 解析 MQTT 下发的 JSON 命令（open / status / open_all / read_all），
 *      调用 lock_core 层执行锁操作，把结果组织成 JSON 回复发布回 MQTT。
 *   2. 把 lock_core 上抛的 0x85 门状态变化事件，组织成 JSON 发布到 event topic。
 *
 * 这是 MQTT 层与锁命令核心层（lock_core）之间的粘合层。
 */

typedef struct {
    mqtt_client_t *mqtt;
    const char *event_topic;   /* 0x85 事件/心跳上报的 topic */
    int box_count;
} cmd_ctx_t;

/* 回复 topic（由 locker_agent 用常量传入） */
typedef struct {
    const char *reply_topic;
} cmd_topic_t;

/*
 * MQTT 消息到达入口（由 mqtt_client 回调调用）。
 * 解析 payload 中的 cmd/box/id，执行锁操作并 publish 回复。
 */
void cmd_handle_message(cmd_ctx_t *ctx, const cmd_topic_t *topics,
                        const char *topic, const char *payload,
                        int payload_len);

/*
 * 0x85 门状态变化事件回调（由 lock_core 在接收线程里调用）。
 * channel=通道号, lock_status=0关/1开。
 */
void cmd_handle_event(cmd_ctx_t *ctx, uint8_t channel, uint8_t lock_status);

/*
 * 上报全量锁状态（心跳）。
 */
void cmd_report_all_status(cmd_ctx_t *ctx);

#endif /* _APP_CMD_H */