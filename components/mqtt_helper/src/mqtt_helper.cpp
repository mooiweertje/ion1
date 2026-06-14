#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "mqtt_client.h"
#include "mqtt_helper.h"

static const char *TAG = "mqtt_helper";

static const int MQTT_CONNECTED_BIT = BIT0;
static const int MQTT_PUB_STATS_BIT = BIT1;

static const int STATS_INTERVAL_SEC = 2;

// MQTT details
static const char* mqtt_server = "mqtt://raspberrypi.fritz.box";

static EventGroupHandle_t mqttEventGroup;

static char deviceTopic[30] = {};

static char deviceName[32] = {};

// I expect event loop events to come from one task, so no need for volatile/semaphores
static uint32_t reconnectCount = -1;

static esp_mqtt_client_handle_t mqttClient;

static TimerHandle_t statsTimer;

static topic_subscriber_t topicSubscriber;
static message_handler_t messageHandler;
static any_message_handler_t anyMessageHandler;

void publishDevProp(const char *deviceProperty, const char *value) {
	char topic[50];
	snprintf(topic, sizeof(topic), "%s/$%s", deviceTopic, deviceProperty);
    esp_mqtt_client_publish(mqttClient, topic, value, 0, 1, 1);
}

void publishNodeProp(const char *nodeId, const char *property, const char *value) {
	char topic[60];
	snprintf(topic, sizeof(topic), "%s/%s/%s", deviceTopic, nodeId, property);
    esp_mqtt_client_publish(mqttClient, topic, value, 0, 1, 1);
}

void subscribeDevTopic(const char *subTopic) {
	char topic[50];
	snprintf(topic, sizeof(topic), "%s/%s", deviceTopic, subTopic);
    esp_mqtt_client_subscribe(mqttClient, topic, 2);
}

void subscribeTopic(const char *topic) {
    esp_mqtt_client_subscribe(mqttClient, topic, 2);
}

static void publishStatsTask(void * pvParameter) {
    while(true) {
        xEventGroupWaitBits(mqttEventGroup, MQTT_PUB_STATS_BIT, true, true, portMAX_DELAY);

        char uptime[16];
        snprintf(uptime, sizeof(uptime), "%llu", esp_timer_get_time() / 1000000);
        publishDevProp("stats/uptime", uptime);

        char heap[16];
        snprintf(heap, sizeof(heap), "%lu", esp_get_free_heap_size());
        publishDevProp("stats/freeheap", heap);

        char reconnects[10];
        snprintf(reconnects, sizeof(reconnects), "%lu", reconnectCount);
        publishDevProp("stats/reconnects", reconnects);

        char signal[16];
        wifi_ap_record_t wifidata = {};
        ESP_ERROR_CHECK(esp_wifi_sta_get_ap_info(&wifidata));
        // NOTE: Not sure what the scale is here, let's call lowest 0% and highest 100%
        snprintf(signal, sizeof(signal), "%d", ((wifidata.rssi - INT8_MIN)  * 100) / 255);
        publishDevProp("stats/signal", signal);
    }
}

static void publishStatsCallback(TimerHandle_t xTimer) {
    xEventGroupSetBits(mqttEventGroup, MQTT_PUB_STATS_BIT);
}

static void handleConnected() {
    reconnectCount++;

    publishDevProp("homie", "3.0.1");

    publishDevProp("state", "init");

    publishDevProp("name", deviceName);

    esp_netif_ip_info_t ipInfo = {}; 
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");    
    esp_netif_get_ip_info(netif, &ipInfo);
	char ipValue[16];
    snprintf(ipValue, sizeof(ipValue), IPSTR, IP2STR(&ipInfo.ip));
    publishDevProp("localip", ipValue);

    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
	char macValue[18];
    snprintf(macValue, sizeof(macValue), "%02X:%02X:%02X:%02X:%02X:%02X",  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    publishDevProp("mac", macValue);

    const esp_app_desc_t * appDesc = esp_app_get_description();
    publishDevProp("fw/name", appDesc->project_name);
    publishDevProp("fw/version", appDesc->version);

    publishDevProp("nodes", "TODO:ir");
    
    publishDevProp("implementation", "custom");

    publishDevProp("stats", "uptime,freeheap,signal,reconnects");

	char interval[10];
    snprintf(interval, sizeof(interval), "%d", STATS_INTERVAL_SEC);
    publishDevProp("stats/interval", interval);

    // Repeat on scheduler, but send current values once now
    xEventGroupSetBits(mqttEventGroup, MQTT_PUB_STATS_BIT);

    topicSubscriber();

    xTimerStart(statsTimer, 0);

    publishDevProp("state", "ready");
}

static void handleDisconnected() {
    xTimerStop(statsTimer, 0);
}

// Topic will be modified by strtok_r
static void handleMessage(char* topic, const char* data) {
    if(strncmp(topic, deviceTopic, strlen(deviceTopic)) != 0) {
        ESP_LOGE(TAG, "MQTT Topic '%s' does not start with our root topic '%s'", topic, deviceTopic);
        return;
    }

    const char *propLevel1 = NULL;
    const char *propLevel2 = NULL;
    const char *propLevel3 = NULL;

    char *token = NULL;
    char *saveptr;
    for (uint8_t pos = 0; (pos == 0 || token) && pos < 5; pos++) {
        token = strtok_r(pos == 0 ? topic : NULL, "/", &saveptr);
        switch (pos) {
            case 2:
                propLevel1 = token;
                break;
            case 3:
                propLevel2 = token;
                break;
            case 4:
                propLevel3 = token;
        }
    }

    if(propLevel1 == NULL) {
        ESP_LOGE(TAG, "MQTT Topic does not include any property");
        return;
    }

    messageHandler(propLevel1, propLevel2, propLevel3, data);
}


static void mqttEventHandler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    // esp_mqtt_client_handle_t client = event->client;
    switch (event->event_id) {
        case MQTT_EVENT_BEFORE_CONNECT:
            ESP_LOGD(TAG, "MQTT_EVENT_BEFORE_CONNECT");
            break;
        case MQTT_EVENT_CONNECTED:
            ESP_LOGD(TAG, "MQTT_EVENT_CONNECTED");
            handleConnected();
            xEventGroupSetBits(mqttEventGroup, MQTT_CONNECTED_BIT);
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGD(TAG, "MQTT_EVENT_DISCONNECTED");
            handleDisconnected();
            xEventGroupClearBits(mqttEventGroup, MQTT_CONNECTED_BIT);
            break;
        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGD(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_UNSUBSCRIBED:
            ESP_LOGD(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_PUBLISHED:
            ESP_LOGD(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_DATA:
            ESP_LOGD(TAG, "MQTT_EVENT_DATA");
            printf("TOPIC=%.*s\n", event->topic_len, event->topic);
            printf("DATA=%.*s\n", event->data_len, event->data);

            {
                char topic[50] = {};
                if(event->topic_len >= sizeof(topic)) {
                    ESP_LOGE(TAG, "MQTT Topic length %d exceeds maximum %d", event->topic_len, sizeof(topic));
                    break;
                }
                snprintf(topic, sizeof(topic), "%.*s", event->topic_len, event->topic);

                char data[50] = {};
                if(event->data_len >= sizeof(data)) {
                    ESP_LOGE(TAG, "MQTT Data length %d exceeds maximum %d", event->data_len, sizeof(data));
                    break;
                }
                snprintf(data, sizeof(data), "%.*s", event->data_len, event->data);
                if(anyMessageHandler == NULL || !anyMessageHandler(topic,data)) {
                    handleMessage(topic, data);
                }
            }
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT_EVENT_ERROR");
            break;
        default:
            ESP_LOGI(TAG, "Other event id:%d", event->event_id);
            break;
    }
}

esp_err_t mqttStart(char* name, topic_subscriber_t topicSubscriberArg, message_handler_t messageHandlerArg, any_message_handler_t anyMessageHandlerArg) {
    size_t maxNameLen = sizeof(deviceName) - 1;
    if(strlen(name) > maxNameLen) {
        ESP_LOGE(TAG, "Name length %d exceeds maximum %d", strlen(name), maxNameLen);
        return ESP_FAIL;
    }
    strcpy(deviceName, name);

    if(topicSubscriberArg == NULL) {
        ESP_LOGE(TAG, "Topic subscriber must be set");
        return ESP_FAIL;
    }

    if(messageHandlerArg == NULL) {
        ESP_LOGE(TAG, "Message handler must be set");
        return ESP_FAIL;
    }

    topicSubscriber = topicSubscriberArg;
    messageHandler = messageHandlerArg;
    anyMessageHandler = anyMessageHandlerArg;

    mqttEventGroup = xEventGroupCreate();

    xTaskCreate(&publishStatsTask, "mqtt_stats_task", 3072, NULL, 5, NULL);
    statsTimer = xTimerCreate("StatsTimer", ((STATS_INTERVAL_SEC * 1000) / portTICK_PERIOD_MS), pdTRUE, (void *) 0, publishStatsCallback);

    // Get the default MAC of this ESP32
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_efuse_mac_get_default(mac));

    // Create a topic for this device
    snprintf(deviceTopic, sizeof(deviceTopic), "%s/%02x%02x%02x%02x%02x%02x", "devices", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    char lwt_topic[40];
	snprintf(lwt_topic, sizeof(lwt_topic), "%s/$state", deviceTopic);

    // esp_mqtt_client_init will make copies of all the provided config data
    esp_mqtt_client_config_t mqtt_cfg = {};
    mqtt_cfg.broker.address.uri = mqtt_server;
    mqtt_cfg.session.last_will.topic = lwt_topic;
    mqtt_cfg.session.last_will.msg = "lost";
    mqtt_cfg.session.last_will.qos = 1;
    mqtt_cfg.session.last_will.retain = 1;

    mqttClient = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqttClient, MQTT_EVENT_ANY, mqttEventHandler, NULL);
    esp_mqtt_client_start(mqttClient);

    return ESP_OK;
}

void mqttWait() {
    xEventGroupWaitBits(mqttEventGroup, MQTT_CONNECTED_BIT, false, true, portMAX_DELAY);
}

void mqttPublish(const char *topic, const char *data, int len, int qos, int retain) {
    esp_mqtt_client_publish(mqttClient, topic, data, len, qos, retain);
}
