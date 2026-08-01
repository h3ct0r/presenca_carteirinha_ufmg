#include "mock_freertos.h"

#include <pthread.h>
#include <time.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <deque>
#include <vector>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static unsigned g_delay_scale = 1;

void mock_freertos_set_delay_scale(unsigned scale) { g_delay_scale = scale ? scale : 1; }

// ---- semaphores ------------------------------------------------------------

struct mock_semaphore {
    pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
};

SemaphoreHandle_t xSemaphoreCreateMutex(void) { return new mock_semaphore(); }

BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t) {
    pthread_mutex_lock(&sem->m);
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t sem) {
    pthread_mutex_unlock(&sem->m);
    return pdTRUE;
}

// ---- queues ----------------------------------------------------------------

struct mock_queue {
    pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
    size_t item_size = 0;
    size_t capacity = 0;
    std::deque<std::vector<uint8_t>> items;
};

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size) {
    mock_queue* q = new mock_queue();
    q->capacity = length;
    q->item_size = item_size;
    return q;
}

BaseType_t xQueueSend(QueueHandle_t q, const void* item, TickType_t) {
    pthread_mutex_lock(&q->m);
    if (q->items.size() >= q->capacity) {
        pthread_mutex_unlock(&q->m);
        return pdFALSE;  // full: matches errQUEUE_FULL != pdTRUE
    }
    const uint8_t* p = (const uint8_t*)item;
    q->items.emplace_back(p, p + q->item_size);
    pthread_cond_signal(&q->cv);
    pthread_mutex_unlock(&q->m);
    return pdTRUE;
}

BaseType_t xQueueReceive(QueueHandle_t q, void* item, TickType_t timeout) {
    pthread_mutex_lock(&q->m);
    while (q->items.empty()) {
        if (timeout == portMAX_DELAY) {
            pthread_cond_wait(&q->cv, &q->m);
        } else {
            pthread_mutex_unlock(&q->m);
            return pdFALSE;  // non-blocking (0 and finite timeouts)
        }
    }
    memcpy(item, q->items.front().data(), q->item_size);
    q->items.pop_front();
    pthread_mutex_unlock(&q->m);
    return pdTRUE;
}

// ---- tasks -----------------------------------------------------------------

struct mock_task {
    pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
    uint32_t notify = 0;
};

static thread_local mock_task* t_current_task = nullptr;

struct task_start_ctx {
    TaskFunction_t fn;
    void* arg;
    mock_task* self;
};

static void* task_trampoline(void* raw) {
    task_start_ctx* ctx = (task_start_ctx*)raw;
    t_current_task = ctx->self;
    TaskFunction_t fn = ctx->fn;
    void* arg = ctx->arg;
    delete ctx;
    fn(arg);
    return nullptr;
}

BaseType_t xTaskCreate(TaskFunction_t fn, const char*, uint32_t, void* arg, UBaseType_t,
                       TaskHandle_t* out_handle) {
    mock_task* task = new mock_task();  // leaked on purpose (detached thread)
    task_start_ctx* ctx = new task_start_ctx{fn, arg, task};
    pthread_t th;
    if (pthread_create(&th, nullptr, task_trampoline, ctx) != 0) {
        delete ctx;
        delete task;
        return pdFAIL;
    }
    pthread_detach(th);
    if (out_handle) *out_handle = task;
    return pdPASS;
}

void vTaskDelete(TaskHandle_t task) {
    if (task == nullptr) pthread_exit(nullptr);
    // Deleting another task is not supported by the mock (unused in project).
}

void vTaskDelay(TickType_t ticks) {
    uint64_t total_ms = (uint64_t)ticks * g_delay_scale;
    while (total_ms > 0) {
        uint64_t chunk = total_ms > 500 ? 500 : total_ms;
        usleep((useconds_t)(chunk * 1000));
        total_ms -= chunk;
    }
}

BaseType_t xTaskNotifyGive(TaskHandle_t task) {
    pthread_mutex_lock(&task->m);
    task->notify++;
    pthread_cond_signal(&task->cv);
    pthread_mutex_unlock(&task->m);
    return pdTRUE;
}

uint32_t ulTaskNotifyTake(BaseType_t clear_on_exit, TickType_t timeout) {
    mock_task* task = t_current_task;
    if (!task) return 0;
    pthread_mutex_lock(&task->m);
    while (task->notify == 0) {
        if (timeout == portMAX_DELAY) {
            pthread_cond_wait(&task->cv, &task->m);
        } else {
            pthread_mutex_unlock(&task->m);
            return 0;
        }
    }
    uint32_t val = task->notify;
    task->notify = clear_on_exit ? 0 : task->notify - 1;
    pthread_mutex_unlock(&task->m);
    return val;
}

UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t) {
    // Host tasks are pthreads on the default (8 MB) stack, so there is no
    // FreeRTOS high-water mark to report. Answer "plenty": production code only
    // logs this or checks it against a floor, and a small number here would be a
    // lie that could fail a test for a condition the host cannot have.
    return 4096;  // words
}
