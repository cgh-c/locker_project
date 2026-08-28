#include "mqtt_client.h"

#include <stdio.h>
#include <string.h>

#define MQTT_KEEPALIVE_SEC   20
#define MQTT_QOS_DEFAULT     1

/* ---------- Paho 回调：连接丢失 ---------- */
static void connection_lost(void *context, char *cause)
{
    (void)context;

    printf("\n[MQTT] connection lost\n");
    if (cause) {
        printf("[MQTT] cause: %s\n", cause);
    }
}

/* ---------- Paho 回调：消息到达 ----------
 * 这里只透传，不解析 JSON、不碰业务。 */
static int message_arrived(void *context,
                           char *topic_name,
                           int topic_len,
                           MQTTClient_message *message)
{
    (void)topic_len;

    mqtt_client_t *m = (mqtt_client_t *)context;

    if (m && m->on_msg) {
        m->on_msg(topic_name, (const char *)message->payload,
                  message->payloadlen, m->user);
    } else {
        printf("[MQTT] message dropped (no handler)\n");
    }

    MQTTClient_freeMessage(&message);
    MQTTClient_free(topic_name);
    return 1;
}

int mqtt_client_init(mqtt_client_t *m, const mqtt_config_t *cfg,
                     mqtt_msg_cb cb, void *user)
{
    if (!m || !cfg) return -1;

    memset(m, 0, sizeof(*m));
    m->cfg = *cfg;   /* 保存连接配置副本 */
    m->on_msg = cb;
    m->user = user;

    /* Paho 要求 serverURI 形如 "tcp://host:port" */
    char broker_url[160];
    snprintf(broker_url, sizeof(broker_url), "tcp://%s",
             cfg->broker);

    int rc = MQTTClient_create(&m->client,
                               broker_url,
                               cfg->client_id,
                               MQTTCLIENT_PERSISTENCE_NONE,
                               NULL);
    if (rc != MQTTCLIENT_SUCCESS) {
        printf("[MQTT] create failed, rc=%d\n", rc);
        return -1;
    }

    rc = MQTTClient_setCallbacks(m->client,
                                 m,
                                 connection_lost,
                                 message_arrived,
                                 NULL);
    if (rc != MQTTCLIENT_SUCCESS) {
        printf("[MQTT] set callbacks failed, rc=%d\n", rc);
        MQTTClient_destroy(&m->client);
        return -1;
    }

    return 0;
}

int mqtt_client_connect(mqtt_client_t *m)
{
    if (!m) return -1;

    MQTTClient_connectOptions opts = MQTTClient_connectOptions_initializer;
    opts.keepAliveInterval = MQTT_KEEPALIVE_SEC;
    opts.cleansession = 1;
    opts.username = m->cfg.username;
    opts.password = m->cfg.password;

    printf("[MQTT] connecting to %s\n", m->cfg.broker);

    int rc = MQTTClient_connect(m->client, &opts);
    if (rc != MQTTCLIENT_SUCCESS) {
        printf("[MQTT] connect failed, rc=%d\n", rc);
        return -1;
    }

    printf("[MQTT] connected\n");
    return 0;
}

int mqtt_client_subscribe(mqtt_client_t *m, const char *topic, int qos)
{
    if (!m || !topic) return -1;

    int rc = MQTTClient_subscribe(m->client, topic, qos);
    if (rc != MQTTCLIENT_SUCCESS) {
        printf("[MQTT] subscribe failed, rc=%d\n", rc);
        return -1;
    }
    printf("[MQTT] subscribed: %s\n", topic);
    return 0;
}

int mqtt_client_publish(mqtt_client_t *m, const char *topic,
                        const char *payload, int qos)
{
    if (!m || !topic || !payload) return -1;

    MQTTClient_message msg = MQTTClient_message_initializer;
    MQTTClient_deliveryToken token;

    msg.payload = (void *)payload;
    msg.payloadlen = strlen(payload);
    msg.qos = qos;
    msg.retained = 0;

    int rc = MQTTClient_publishMessage(m->client, topic, &msg, &token);
    if (rc != MQTTCLIENT_SUCCESS) {
        printf("[MQTT] publish failed, rc=%d\n", rc);
        return -1;
    }
    return 0;
}

void mqtt_client_disconnect(mqtt_client_t *m, const char *sub_topic)
{
    if (!m) return;

    if (sub_topic) {
        MQTTClient_unsubscribe(m->client, sub_topic);
    }
    MQTTClient_disconnect(m->client, 1000L);
    MQTTClient_destroy(&m->client);
}