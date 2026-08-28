#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>

#include "../app/app_config.h"
#include "../app/app_cmd.h"
#include "../mqtt/mqtt_client.h"
#include "hal_locker.h"
#include "lock_core.h"

/* MQTT Topic（业务层定义，mqtt 模块不写死） */
#define MQTT_CMD_TOPIC      "locker/locker001/cmd"
#define MQTT_REPLY_TOPIC    "locker/locker001/reply"
#define MQTT_EVENT_TOPIC    "locker/locker001/event"

#define MQTT_QOS         1
#define HEARTBEAT_SEC    30

static volatile int g_running = 1;

static void signal_handler(int sig)
{
    (void)sig;
    g_running = 0;
}

/*
 * 0x85 门状态变化回调（lock_core 在接收线程里调用）。
 * 直接转交命令处理模块发布到 event topic。
 */
static void on_locker_event(uint8_t channel, uint8_t lock_status, void *user)
{
    cmd_ctx_t *cmd = (cmd_ctx_t *)user;
    cmd_handle_event(cmd, channel, lock_status);
}

/* MQTT 消息到达 → 转交命令处理模块 */
static void on_mqtt_message(const char *topic, const char *payload,
                            int payload_len, void *user)
{
    cmd_ctx_t *cmd = (cmd_ctx_t *)user;
    cmd_topic_t topics = { MQTT_REPLY_TOPIC };

    cmd_handle_message(cmd, &topics, topic, payload, payload_len);
}

int main(void)
{
    app_config_t config;
    mqtt_config_t mqtt_cfg;
    mqtt_client_t mqtt;
    cmd_ctx_t cmd;
    int lock_fd;
    int rc;
    time_t last_beat = 0;

    /* ------ 加载配置文件 ------ */
    if (app_config_load("config/locker_config.json", &config) < 0) {
        return -1;
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* ------ 1. 初始化 HAL 串口 ------ */
    printf("[HAL] initializing RS485...\n");
    lock_fd = lock_serial_init(config.serial_device, config.baudrate);
    if (lock_fd < 0) {
        printf("[HAL] initialization failed\n");
        return -1;
    }
    printf("[HAL] initialization success, fd=%d\n", lock_fd);

    /* ------ 2. 初始化 MQTT ------ */
    strcpy(mqtt_cfg.broker, config.broker);
    mqtt_cfg.port = config.port;
    strcpy(mqtt_cfg.username, config.username);
    strcpy(mqtt_cfg.password, config.password);
    strcpy(mqtt_cfg.client_id, config.client_id);

    /* 命令上下文：持有 mqtt 句柄 + 事件 topic */
    cmd.mqtt = &mqtt;
    cmd.event_topic = MQTT_EVENT_TOPIC;
    cmd.box_count = config.box_count;

    if (mqtt_client_init(&mqtt, &mqtt_cfg, on_mqtt_message, &cmd) < 0) {
        printf("[MQTT] init failed\n");
        lock_serial_close(lock_fd);
        return -1;
    }

    /* ------ 3. 初始化锁命令核心层（启动接收线程） ------ */
    if (lock_core_init(lock_fd, on_locker_event, &cmd) < 0) {
        printf("[Core] init failed\n");
        mqtt_client_disconnect(&mqtt, NULL);
        lock_serial_close(lock_fd);
        return -1;
    }

    /* ------ 4. 连接 broker ------ */
    if (mqtt_client_connect(&mqtt) < 0) {
        printf("[MQTT] connect failed\n");
        lock_core_deinit();
        mqtt_client_disconnect(&mqtt, NULL);
        lock_serial_close(lock_fd);
        return -1;
    }

    /* ------ 5. 订阅命令主题 ------ */
    rc = mqtt_client_subscribe(&mqtt, MQTT_CMD_TOPIC, MQTT_QOS);
    if (rc < 0) {
        printf("[MQTT] subscribe failed\n");
        lock_core_deinit();
        mqtt_client_disconnect(&mqtt, NULL);
        lock_serial_close(lock_fd);
        return -1;
    }

    /* ------ 6. 主循环：定时心跳 ------ */
    printf("\n");
    printf("===============================\n");
    printf(" locker_agent running\n");
    printf("===============================\n");

    while (g_running) {
        time_t now = time(NULL);
        if (now - last_beat >= HEARTBEAT_SEC) {
            cmd_report_all_status(&cmd);
            last_beat = now;
        }
        sleep(1);
    }

    /* ------ 7. 退出 ------ */
    printf("\n[APP] shutting down...\n");

    lock_core_deinit();
    mqtt_client_disconnect(&mqtt, MQTT_CMD_TOPIC);
    lock_serial_close(lock_fd);

    printf("[APP] exit\n");
    return 0;
}