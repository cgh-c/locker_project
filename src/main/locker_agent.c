#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>

#include "MQTTClient.h"
#include "hal_locker.h"

#include "cJSON.h"

// 全局串口 fd（供 send_reply 等使用）
static int g_serial_fd = -1;
/* MQTT Topic */
#define MQTT_CMD_TOPIC      "locker/locker001/cmd"
#define MQTT_REPLY_TOPIC    "locker/locker001/reply"

#define MQTT_QOS         1
#define MQTT_TIMEOUT     1000L

static volatile int g_running = 1;

/*
 * 应用上下文
 *
 * MQTT callback 需要同时使用：
 * 1. MQTT client
 * 2. HAL 串口 fd
 */
typedef struct
{
    MQTTClient mqtt_client;
    int lock_fd;
} locker_context_t;


/*
 * Ctrl+C退出
 */
static void signal_handler(int sig)
{
    (void)sig;
    g_running = 0;
}



/*
 * 发布字符串消息
 */
static int mqtt_publish_text(MQTTClient client,
                             const char *topic,
                             const char *payload)
{
    MQTTClient_message msg = MQTTClient_message_initializer;
    MQTTClient_deliveryToken token;
    msg.payload = (void *)payload;
    msg.payloadlen = strlen(payload);
    msg.qos = MQTT_QOS;
    msg.retained = 0;

    int rc = MQTTClient_publishMessage(client, topic, &msg, &token);
    if (rc != MQTTCLIENT_SUCCESS) {
        printf("[MQTT] publish failed, rc=%d\n", rc);
        return -1;
    }
    // 注意：这里不再调用 MQTTClient_waitForCompletion
    return 0;
}

/*
 * 处理 open 命令
 *
 * 输入：
 * open 1
 */
static void handle_open(locker_context_t *ctx, int door_id)
{
    char reply[128];

    printf("[LOCKER] open door %d\n", door_id);

    if (door_id < 1 || door_id > 12)
    {
        snprintf(
            reply,
            sizeof(reply),
            "open %d failed invalid_door_id",
            door_id
        );

        mqtt_publish_text(
            ctx->mqtt_client,
            MQTT_REPLY_TOPIC,
            reply
        );

        return;
    }

    int ret = lock_open_single(
        ctx->lock_fd,
        (uint8_t)door_id,
        LOCK_DEFAULT_TIMEOUT_MS
    );

    if (ret == 0)
    {
        printf("[LOCKER] door %d open success\n", door_id);

        snprintf(
            reply,
            sizeof(reply),
            "open %d ok",
            door_id
        );
    }
    else if (ret == -1)
    {
        printf("[LOCKER] door %d timeout\n", door_id);

        snprintf(
            reply,
            sizeof(reply),
            "open %d failed timeout",
            door_id
        );
    }
    else
    {
        printf(
            "[LOCKER] door %d failed, ret=%d\n",
            door_id,
            ret
        );

        snprintf(
            reply,
            sizeof(reply),
            "open %d failed hal_error",
            door_id
        );
    }

    mqtt_publish_text(
        ctx->mqtt_client,
        MQTT_REPLY_TOPIC,
        reply
    );
}


/*
 * 处理 status 命令
 *
 * 输入：
 * status 1
 */
static void handle_status(locker_context_t *ctx, int door_id)
{
    uint8_t status;

    char reply[128];

    printf(
        "[LOCKER] read door %d status\n",
        door_id
    );

    if (door_id < 1 || door_id > 12)
    {
        snprintf(
            reply,
            sizeof(reply),
            "status %d failed invalid_door_id",
            door_id
        );

        mqtt_publish_text(
            ctx->mqtt_client,
            MQTT_REPLY_TOPIC,
            reply
        );

        return;
    }

    int ret = lock_read_single(
        ctx->lock_fd,
        (uint8_t)door_id,
        &status,
        LOCK_DEFAULT_TIMEOUT_MS
    );

    if (ret == 0)
    {
        const char *status_str;

        if (status == LOCK_STATUS_OPEN)
        {
            status_str = "open";
        }
        else
        {
            status_str = "closed";
        }

        printf(
            "[LOCKER] door %d status = %s\n",
            door_id,
            status_str
        );

        snprintf(
            reply,
            sizeof(reply),
            "status %d ok %s",
            door_id,
            status_str
        );
    }
    else if (ret == -1)
    {
        printf(
            "[LOCKER] read door %d timeout\n",
            door_id
        );

        snprintf(
            reply,
            sizeof(reply),
            "status %d failed timeout",
            door_id
        );
    }
    else
    {
        printf(
            "[LOCKER] read door %d failed, ret=%d\n",
            door_id,
            ret
        );

        snprintf(
            reply,
            sizeof(reply),
            "status %d failed hal_error",
            door_id
        );
    }

    mqtt_publish_text(
        ctx->mqtt_client,
        MQTT_REPLY_TOPIC,
        reply
    );
}

/*
 * 发送 JSON 格式的回复
 * id   : 请求中的 id
 * code : 0 成功, 非0 失败
 * msg  : 描述字符串
 * status: -1 表示无状态, 0/1 表示锁状态 (仅在 status 命令时使用)
 */
void send_reply(int id, int code, const char *msg, int status, int box, locker_context_t *ctx) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", id);
    cJSON_AddNumberToObject(root, "code", code);
    cJSON_AddStringToObject(root, "msg", msg);
    cJSON_AddNumberToObject(root, "box", box);  // 新增：返回 box 号
    
    if (status >= 0) {
        cJSON *data = cJSON_CreateObject();
        cJSON_AddNumberToObject(data, "status", status);
        cJSON_AddItemToObject(root, "data", data);
    }
    char *json_str = cJSON_Print(root);
    mqtt_publish_text(ctx->mqtt_client, MQTT_REPLY_TOPIC, json_str);
    free(json_str);
    cJSON_Delete(root);
}

/*
 * 上报全量锁状态（心跳）
 */
void report_all_status(int fd, locker_context_t *ctx) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "event", "heartbeat");
    cJSON *boxes = cJSON_CreateArray();
    for (int i = 1; i <= 12; i++) {
        uint8_t status;
        if (lock_read_single(fd, i, &status, 500) == 0) {
            cJSON *item = cJSON_CreateObject();
            cJSON_AddNumberToObject(item, "id", i);
            cJSON_AddNumberToObject(item, "status", status);
            cJSON_AddItemToArray(boxes, item);
        }
    }
    cJSON_AddItemToObject(root, "boxes", boxes);
    char *json_str = cJSON_Print(root);
    mqtt_publish_text(ctx->mqtt_client, "locker/locker001/event", json_str);
    free(json_str);
    cJSON_Delete(root);
}

static int message_arrived(void *context,
                           char *topic_name,
                           int topic_len,
                           MQTTClient_message *message)
{
    locker_context_t *ctx = (locker_context_t *)context;

    char payload[256];
    int len = message->payloadlen;
    if (len >= sizeof(payload)) len = sizeof(payload) - 1;
    memcpy(payload, message->payload, len);
    payload[len] = '\0';

    printf("\n================================\n");
    printf("[MQTT] message arrived\n");
    if (topic_len == 0) printf("[MQTT] topic   : %s\n", topic_name);
    printf("[MQTT] payload : %s\n", payload);
    printf("================================\n");

    // ------ 解析 JSON 命令 ------
    cJSON *root = cJSON_Parse(payload);
    if (!root) {
        printf("[APP] JSON parse failed\n");
        mqtt_publish_text(ctx->mqtt_client, MQTT_REPLY_TOPIC, "{\"code\":-1,\"msg\":\"invalid json\"}");
        goto exit;
    }

    cJSON *cmd_obj = cJSON_GetObjectItem(root, "cmd");
    cJSON *box_obj = cJSON_GetObjectItem(root, "box");
    cJSON *id_obj = cJSON_GetObjectItem(root, "id");
    cJSON *ts_obj = cJSON_GetObjectItem(root, "ts");
    // 打印 cmd（字符串）
    if (cmd_obj && cJSON_IsString(cmd_obj)) {
        printf("cmd = %s\n", cmd_obj->valuestring);
    } else {
        printf("cmd = (missing or not string)\n");
    }

    // 打印 box（整数）
    if (box_obj && cJSON_IsNumber(box_obj)) {
        printf("box = %d\n", box_obj->valueint);
    } else {
        printf("box = (missing or not number)\n");
    }

    // 打印 id（整数）
    if (id_obj && cJSON_IsNumber(id_obj)) {
        printf("id = %d\n", id_obj->valueint);
    } else {
        printf("id = (missing or not number)\n");
    }

    // 打印 ts（整数，时间戳）
    if (ts_obj && cJSON_IsNumber(ts_obj)) {
        printf("ts = %lld\n", (long long)ts_obj->valuedouble); // 或用 valueint 如果整数不大
    } else {
        printf("ts = (missing or not number)\n");
    }
    // 检查 cmd 必须存在
    if (!cmd_obj) {
        printf("[APP] missing cmd\n");
        mqtt_publish_text(ctx->mqtt_client, MQTT_REPLY_TOPIC, "{\"code\":-1,\"msg\":\"missing cmd\"}");
        cJSON_Delete(root);
        goto exit;
    }

    // 获取 id，优先用 id，否则用 ts，否则用随机数
    int id = 0;
    if (id_obj) {
        id = id_obj->valueint;
    } else if (ts_obj) {
        id = ts_obj->valueint;
    } else {
        id = (int)time(NULL);  // 简单用时间戳
    }

    const char *cmd = cmd_obj->valuestring;
    int box = box_obj ? box_obj->valueint : 0;

    // ------ 执行命令 ------
    int ret = -1;
    uint8_t status = 0;

    if (strcmp(cmd, "open") == 0) {
        ret = lock_open_single(ctx->lock_fd, box, 500);
        send_reply(id, ret == 0 ? 0 : -1, ret == 0 ? "success" : "open failed", -1, box, ctx);
    } else if (strcmp(cmd, "status") == 0) {
        ret = lock_read_single(ctx->lock_fd, box, &status, 500);
        send_reply(id, ret == 0 ? 0 : -1, ret == 0 ? "success" : "read failed", ret == 0 ? status : -1, box, ctx);
    } else if (strcmp(cmd, "open_all") == 0) {
        ret = lock_open_all(ctx->lock_fd, 500);
        send_reply(id, ret == 0 ? 0 : -1, ret == 0 ? "all opened" : "open_all failed", -1, box, ctx);
    } else {
        printf("[APP] unknown command: %s\n", cmd);
        mqtt_publish_text(ctx->mqtt_client, MQTT_REPLY_TOPIC, "{\"code\":-1,\"msg\":\"unknown command\"}");
    }

    cJSON_Delete(root);

exit:
    MQTTClient_freeMessage(&message);
    MQTTClient_free(topic_name);
    return 1;
}

/*
 * MQTT连接丢失
 */
static void connection_lost(void *context,
                            char *cause)
{
    (void)context;

    printf("\n[MQTT] connection lost\n");

    if (cause)
    {
        printf("[MQTT] cause: %s\n", cause);
    }
}


int main(void)
{
        typedef struct {
        char broker[128];
        int port;
        char username[32];
        char password[32];
        char client_id[32];
        char serial_device[64];
        int baudrate;
        int timeout_ms;
        int board_addr;
        int box_count;
    } AppConfig;

    AppConfig config;

    // 加载配置文件
    FILE *fp = fopen("config/locker_config.json", "r");
    if (!fp) {
        printf("[ERROR] Cannot open config/locker_config.json\n");
        return -1;
    }
    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *json_str = malloc(fsize + 1);
    fread(json_str, 1, fsize, fp);
    json_str[fsize] = '\0';
    fclose(fp);

    cJSON *root = cJSON_Parse(json_str);
    free(json_str);
    if (!root) {
        printf("[ERROR] JSON parse failed\n");
        return -1;
    }

    cJSON *mqtt = cJSON_GetObjectItem(root, "mqtt");
    cJSON *serial = cJSON_GetObjectItem(root, "serial");
    cJSON *locker = cJSON_GetObjectItem(root, "locker");

    strcpy(config.broker, cJSON_GetObjectItem(mqtt, "broker")->valuestring);
    config.port = cJSON_GetObjectItem(mqtt, "port")->valueint;
    strcpy(config.username, cJSON_GetObjectItem(mqtt, "username")->valuestring);
    strcpy(config.password, cJSON_GetObjectItem(mqtt, "password")->valuestring);
    strcpy(config.client_id, cJSON_GetObjectItem(mqtt, "client_id")->valuestring);
    strcpy(config.serial_device, cJSON_GetObjectItem(serial, "device")->valuestring);
    config.baudrate = cJSON_GetObjectItem(serial, "baudrate")->valueint;
    config.timeout_ms = cJSON_GetObjectItem(serial, "timeout_ms")->valueint;
    config.board_addr = cJSON_GetObjectItem(locker, "board_addr")->valueint;
    config.box_count = cJSON_GetObjectItem(locker, "box_count")->valueint;

    cJSON_Delete(root);

    MQTTClient client;

    MQTTClient_connectOptions conn_opts =
        MQTTClient_connectOptions_initializer;

    locker_context_t ctx;

    int rc;


    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);


    /*
     * 1. 初始化HAL
     */
    printf("[HAL] initializing RS485...\n");

    int lock_fd = lock_serial_init(
        config.serial_device,
        config.baudrate
    );

    if (lock_fd < 0)
    {
        printf("[HAL] initialization failed\n");
        return -1;
    }

    printf(
        "[HAL] initialization success, fd=%d\n",
        lock_fd
    );


    /*
     * 2. 创建MQTT客户端
     */
    rc = MQTTClient_create(
        &client,
        config.broker,
        config.client_id,
        MQTTCLIENT_PERSISTENCE_NONE,
        NULL
    );

    if (rc != MQTTCLIENT_SUCCESS)
    {
        printf(
            "[MQTT] create failed, rc=%d\n",
            rc
        );

        lock_serial_close(lock_fd);
        return -1;
    }


    /*
     * context提供给callback
     */
    ctx.mqtt_client = client;
    ctx.lock_fd = lock_fd;


    /*
     * 3. 注册callback
     */
    rc = MQTTClient_setCallbacks(
        client,
        &ctx,
        connection_lost,
        message_arrived,
        NULL
    );

    if (rc != MQTTCLIENT_SUCCESS)
    {
        printf(
            "[MQTT] set callbacks failed, rc=%d\n",
            rc
        );

        MQTTClient_destroy(&client);
        lock_serial_close(lock_fd);

        return -1;
    }


    /*
     * 4. 连接Broker
     */
    conn_opts.keepAliveInterval = 20;
    conn_opts.cleansession = 1;
    conn_opts.username = config.username;
    conn_opts.password = config.password;
    printf(
        "[MQTT] connecting to %s\n",
        config.broker
    );

    rc = MQTTClient_connect(
        client,
        &conn_opts
    );

    if (rc != MQTTCLIENT_SUCCESS)
    {
        printf(
            "[MQTT] connect failed, rc=%d\n",
            rc
        );

        MQTTClient_destroy(&client);
        lock_serial_close(lock_fd);

        return -1;
    }

    printf("[MQTT] connected\n");


    /*
     * 5. 订阅命令Topic
     */
    rc = MQTTClient_subscribe(
        client,
        MQTT_CMD_TOPIC,
        MQTT_QOS
    );

    if (rc != MQTTCLIENT_SUCCESS)
    {
        printf(
            "[MQTT] subscribe failed, rc=%d\n",
            rc
        );

        MQTTClient_disconnect(
            client,
            1000
        );

        MQTTClient_destroy(&client);
        lock_serial_close(lock_fd);

        return -1;
    }

    printf(
        "[MQTT] subscribed: %s\n",
        MQTT_CMD_TOPIC
    );


    /*
     * 6. 主循环
     *
     * MQTT消息由callback处理。
     */
    printf("\n");
    printf("===============================\n");
    printf(" locker_agent running\n");
    printf("===============================\n");

    while (g_running)
    {
        sleep(1);
    }


    /*
     * 7. 退出
     */
    printf("\n[APP] shutting down...\n");

    MQTTClient_unsubscribe(
        client,
        MQTT_CMD_TOPIC
    );

    MQTTClient_disconnect(
        client,
        1000
    );

    MQTTClient_destroy(
        &client
    );

    lock_serial_close(
        lock_fd
    );

    printf("[APP] exit\n");

    return 0;
}
