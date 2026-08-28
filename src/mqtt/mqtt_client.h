#ifndef _MQTT_CLIENT_H
#define _MQTT_CLIENT_H

#include "MQTTClient.h"

/*
 * MQTT 客户端封装（基于 Paho MQTT C 库，paho-mqtt3c）
 *
 * 职责边界：
 *   - 只负责 MQTT 生命周期：create / setCallbacks / connect /
 *     subscribe / publish / unsubscribe / disconnect / destroy
 *   - 收到消息后只把 topic + payload 原样转交给上层回调，
 *     不解析 JSON、不关心业务命令、不碰锁。
 *
 * topic 不在此模块内写死，全部由调用方通过参数传入。
 */

/* MQTT 连接配置（由调用方填写） */
typedef struct {
    char broker[128];
    int  port;
    char username[32];
    char password[32];
    char client_id[32];
} mqtt_config_t;

/* 收到消息的上层回调 */
typedef void (*mqtt_msg_cb)(const char *topic, const char *payload,
                            int payload_len, void *user);

typedef struct {
    MQTTClient client;
    mqtt_config_t cfg;   /* init 时保存的连接配置副本 */
    mqtt_msg_cb on_msg;
    void *user;
} mqtt_client_t;

/* 初始化：create + setCallbacks。成功返回 0，失败返回 -1。 */
int mqtt_client_init(mqtt_client_t *m, const mqtt_config_t *cfg,
                     mqtt_msg_cb cb, void *user);

/* 连接 broker。成功返回 0，失败返回 -1。 */
int mqtt_client_connect(mqtt_client_t *m);

/* 订阅主题。成功返回 0，失败返回 -1。 */
int mqtt_client_subscribe(mqtt_client_t *m, const char *topic, int qos);

/* 发布文本消息（payload 为 NUL 结尾字符串）。成功返回 0，失败返回 -1。 */
int mqtt_client_publish(mqtt_client_t *m, const char *topic,
                        const char *payload, int qos);

/* 断开并销毁客户端（unsubscribe + disconnect + destroy）。 */
void mqtt_client_disconnect(mqtt_client_t *m, const char *sub_topic);

#endif /* _MQTT_CLIENT_H */