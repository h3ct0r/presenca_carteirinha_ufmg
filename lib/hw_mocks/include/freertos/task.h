#pragma once

#include "freertos/FreeRTOS.h"

typedef struct mock_task* TaskHandle_t;
typedef void (*TaskFunction_t)(void*);
typedef uint32_t StackType_t;  // real FreeRTOS: the stack word type

BaseType_t xTaskCreate(TaskFunction_t fn, const char* name, uint32_t stack_words, void* arg,
                       UBaseType_t priority, TaskHandle_t* out_handle);
void vTaskDelete(TaskHandle_t task);  // NULL = delete calling task
void vTaskDelay(TickType_t ticks);
BaseType_t xTaskNotifyGive(TaskHandle_t task);
uint32_t ulTaskNotifyTake(BaseType_t clear_on_exit, TickType_t timeout);
// Unused stack, in words. Host tasks are pthreads with no measurable FreeRTOS
// stack, so this reports a large constant: callers only log it or compare it
// against a threshold, and neither is meaningful off-device.
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task);
