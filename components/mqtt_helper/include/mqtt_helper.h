#pragma once

typedef void (* topic_subscriber_t)();
typedef void (* message_handler_t)(const char* topic1, const char* topic2, const char* topic3, const char* data);
typedef bool (* any_message_handler_t)(const char* topic, const char* data);

esp_err_t mqttStart(char* name, topic_subscriber_t topicSubscriberArg, message_handler_t messageHandlerArg, any_message_handler_t anyMessageHandlerArg);
void mqttWait();
void mqttPublish(const char *topic, const char *data, int len, int qos, int retain);
void publishDevProp(const char *deviceProperty, const char *value);
void publishNodeProp(const char *nodeId, const char *property, const char *value);
void subscribeDevTopic(const char *subTopic);
void subscribeTopic(const char *subTopic);
