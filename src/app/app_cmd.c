#include "app_cmd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "lock_core.h"

#define MQTT_QOS   1

static void publish_json(mqtt_client_t *mqtt, const char *topic, cJSON *root)
{
    char *json_str = cJSON_Print(root);
    mqtt_client_publish(mqtt, topic, json_str, MQTT_QOS);
    free(json_str);
}

/*
 * 组织并发布命令回复。
 * status: -1 表示无状态, 0/1 表示锁状态（仅 status 命令时使用）
 */
static void send_reply(cmd_ctx_t *ctx, const char *reply_topic,
                       int id, int code, const char *msg, int status, int box)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", id);
    cJSON_AddNumberToObject(root, "code", code);
    cJSON_AddStringToObject(root, "msg", msg);
    cJSON_AddNumberToObject(root, "box", box);

    if (status >= 0) {
        cJSON *data = cJSON_CreateObject();
        cJSON_AddNumberToObject(data, "status", status);
        cJSON_AddItemToObject(root, "data", data);
    }

    publish_json(ctx->mqtt, reply_topic, root);
    cJSON_Delete(root);
}

/*
 * 组织并发布 read_all 命令的回复：把全部锁状态放进 data.boxes 数组。
 */
static void send_read_all_reply(cmd_ctx_t *ctx, const char *reply_topic,
                                int id, const uint8_t *statuses, int count)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();
    cJSON *boxes = cJSON_CreateArray();

    cJSON_AddNumberToObject(root, "id", id);
    cJSON_AddNumberToObject(root, "code", 0);
    cJSON_AddStringToObject(root, "msg", "success");

    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "id", i + 1);
        cJSON_AddNumberToObject(item, "status", statuses[i]);
        cJSON_AddItemToArray(boxes, item);
    }
    cJSON_AddItemToObject(data, "boxes", boxes);
    cJSON_AddItemToObject(root, "data", data);

    publish_json(ctx->mqtt, reply_topic, root);
    cJSON_Delete(root);
}

void cmd_handle_message(cmd_ctx_t *ctx, const cmd_topic_t *topics,
                        const char *topic, const char *raw_payload,
                        int payload_len)
{
    /* Paho payload 不保证 NUL 结尾，拷贝一份并补 '\0' */
    char payload[256];
    int len = payload_len;
    if (len >= (int)sizeof(payload)) len = (int)sizeof(payload) - 1;
    memcpy(payload, raw_payload, len);
    payload[len] = '\0';

    printf("\n================================\n");
    printf("[MQTT] message arrived\n");
    if (topic) printf("[MQTT] topic   : %s\n", topic);
    printf("[MQTT] payload : %s\n", payload);
    printf("================================\n");

    cJSON *root = cJSON_Parse(payload);
    if (!root) {
        printf("[APP] JSON parse failed\n");
        mqtt_client_publish(ctx->mqtt, topics->reply_topic,
                            "{\"code\":-1,\"msg\":\"invalid json\"}", MQTT_QOS);
        return;
    }

    cJSON *cmd_obj = cJSON_GetObjectItem(root, "cmd");
    cJSON *box_obj = cJSON_GetObjectItem(root, "box");
    cJSON *id_obj = cJSON_GetObjectItem(root, "id");
    cJSON *ts_obj = cJSON_GetObjectItem(root, "ts");

    if (!cmd_obj) {
        printf("[APP] missing cmd\n");
        mqtt_client_publish(ctx->mqtt, topics->reply_topic,
                            "{\"code\":-1,\"msg\":\"missing cmd\"}", MQTT_QOS);
        cJSON_Delete(root);
        return;
    }

    /* id 优先用 id 字段，其次 ts，最后时间戳 */
    int id = 0;
    if (id_obj) {
        id = id_obj->valueint;
    } else if (ts_obj) {
        id = ts_obj->valueint;
    } else {
        id = (int)time(NULL);
    }

    const char *cmd = cmd_obj->valuestring;
    int box = box_obj ? box_obj->valueint : 0;

    int ret = -1;
    uint8_t status = 0;

    if (strcmp(cmd, "open") == 0) {
        ret = lock_core_open_single(box, 500);
        send_reply(ctx, topics->reply_topic, id, ret == 0 ? 0 : -1,
                   ret == 0 ? "success" : "open failed", -1, box);
    } else if (strcmp(cmd, "status") == 0) {
        ret = lock_core_read_single(box, &status, 500);
        send_reply(ctx, topics->reply_topic, id, ret == 0 ? 0 : -1,
                   ret == 0 ? "success" : "read failed",
                   ret == 0 ? status : -1, box);
    } else if (strcmp(cmd, "open_all") == 0) {
        ret = lock_core_open_all(500);
        send_reply(ctx, topics->reply_topic, id, ret == 0 ? 0 : -1,
                   ret == 0 ? "all opened" : "open_all failed", -1, box);
    } else if (strcmp(cmd, "read_all") == 0) {
        uint8_t statuses[16];
        int count = 0;
        ret = lock_core_read_all(statuses, sizeof(statuses), &count, 500);
        if (ret == 0) {
            send_read_all_reply(ctx, topics->reply_topic, id, statuses, count);
        } else {
            send_reply(ctx, topics->reply_topic, id, -1, "read_all failed", -1, box);
        }
    } else {
        printf("[APP] unknown command: %s\n", cmd);
        mqtt_client_publish(ctx->mqtt, topics->reply_topic,
                            "{\"code\":-1,\"msg\":\"unknown command\"}", MQTT_QOS);
    }

    cJSON_Delete(root);
}

void cmd_handle_event(cmd_ctx_t *ctx, uint8_t channel, uint8_t lock_status)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "event", "lock_change");
    cJSON_AddNumberToObject(root, "box", channel);
    cJSON_AddNumberToObject(root, "status", lock_status);

    publish_json(ctx->mqtt, ctx->event_topic, root);
    cJSON_Delete(root);
}

void cmd_report_all_status(cmd_ctx_t *ctx)
{
    uint8_t statuses[16];
    int count = 0;

    /* 一次 read_all 拿到全部锁状态，比逐把 read_single 高效 */
    if (lock_core_read_all(statuses, sizeof(statuses), &count, 500) != 0) {
        return;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "event", "heartbeat");
    cJSON *boxes = cJSON_CreateArray();

    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "id", i + 1);
        cJSON_AddNumberToObject(item, "status", statuses[i]);
        cJSON_AddItemToArray(boxes, item);
    }

    cJSON_AddItemToObject(root, "boxes", boxes);
    publish_json(ctx->mqtt, ctx->event_topic, root);
    cJSON_Delete(root);
}