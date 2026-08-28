#ifndef _APP_CONFIG_H
#define _APP_CONFIG_H

/*
 * 应用配置模块
 *
 * 职责：把 config/locker_config.json 解析成强类型结构体。
 * 使用 cJSON，是整个工程里唯一直接碰配置文件的模块。
 */

typedef struct {
    char broker[128];
    int  port;
    char username[32];
    char password[32];
    char client_id[32];
    char serial_device[64];
    int  baudrate;
    int  timeout_ms;
    int  board_addr;
    int  box_count;
} app_config_t;

/*
 * 加载并解析配置文件。
 * path 不存在或字段缺失时返回 -1，成功返回 0。
 */
int app_config_load(const char *path, app_config_t *cfg);

#endif /* _APP_CONFIG_H */