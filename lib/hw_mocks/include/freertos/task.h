#pragma once

#include "freertos/FreeRTOS.h"

typedef struct mock_task* TaskHandle_t;
typedef void (*TaskFunction_t)(void*);

BaseType_t xTaskCreate(TaskFunction_t fn, const char* name, uint32_t stack_words, void* arg,
                       UBaseType_t priority, TaskHandle_t* out_handle);
void vTaskDelete(TaskHandle_t task);  // NULL = delete calling task
void vTaskDelay(TickType_t ticks);
BaseType_t xTaskNotifyGive(TaskHandle_t task);
uint32_t ulTaskNotifyTake(BaseType_t clear_on_exit, TickType_t timeout);
