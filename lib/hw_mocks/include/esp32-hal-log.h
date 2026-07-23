#pragma once

// Native stand-in for Arduino-ESP32 logging: plain printf.

#include <stdio.h>

#define MOCK_LOG(lvl, tag, fmt, ...) printf("[" lvl "][%s] " fmt "\n", tag, ##__VA_ARGS__)

#define ESP_LOGE(tag, fmt, ...) MOCK_LOG("E", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) MOCK_LOG("W", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) MOCK_LOG("I", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) MOCK_LOG("D", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGV(tag, fmt, ...) MOCK_LOG("V", tag, fmt, ##__VA_ARGS__)
