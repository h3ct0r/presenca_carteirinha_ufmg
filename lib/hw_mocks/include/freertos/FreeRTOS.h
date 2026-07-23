#pragma once

// Native FreeRTOS shim: just the types/macros the project uses. The
// primitives (queue/semaphore/task) are pthread-backed, see mock_freertos.cpp.

#include <assert.h>
#include <stdint.h>

typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef unsigned int UBaseType_t;

#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define pdFAIL 0
#define portMAX_DELAY 0xFFFFFFFFu
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define configASSERT(x) assert(x)
