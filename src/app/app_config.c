#include "app_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

int app_config_load(const char *path, app_config_t *cfg)
{
    FILE *fp;
    long fsize;
    char *json_str;
    cJSON *root, *mqtt, *serial, *locker;

    if (path == NULL || cfg == NULL) return -1;

    fp = fopen(path, "r");
    if (!fp) {
        printf("[CONFIG] Cannot open %s\n", path);
        return -1;
    }
    fseek(fp, 0, SEEK_END);
    fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    json_str = malloc(fsize + 1);
    fread(json_str, 1, fsize, fp);
    json_str[fsize] = '\0';
    fclose(fp);

    root = cJSON_Parse(json_str);
    free(json_str);
    if (!root) {
        printf("[CONFIG] JSON parse failed\n");
        return -1;
    }

    mqtt = cJSON_GetObjectItem(root, "mqtt");
    serial = cJSON_GetObjectItem(root, "serial");
    locker = cJSON_GetObjectItem(root, "locker");

    if (!mqtt || !serial || !locker) {
        printf("[CONFIG] missing section: mqtt/serial/locker\n");
        cJSON_Delete(root);
        return -1;
    }

    strcpy(cfg->broker, cJSON_GetObjectItem(mqtt, "broker")->valuestring);
    cfg->port = cJSON_GetObjectItem(mqtt, "port")->valueint;
    strcpy(cfg->username, cJSON_GetObjectItem(mqtt, "username")->valuestring);
    strcpy(cfg->password, cJSON_GetObjectItem(mqtt, "password")->valuestring);
    strcpy(cfg->client_id, cJSON_GetObjectItem(mqtt, "client_id")->valuestring);

    strcpy(cfg->serial_device, cJSON_GetObjectItem(serial, "device")->valuestring);
    cfg->baudrate = cJSON_GetObjectItem(serial, "baudrate")->valueint;
    cfg->timeout_ms = cJSON_GetObjectItem(serial, "timeout_ms")->valueint;

    cfg->board_addr = cJSON_GetObjectItem(locker, "board_addr")->valueint;
    cfg->box_count = cJSON_GetObjectItem(locker, "box_count")->valueint;

    cJSON_Delete(root);
    return 0;
}