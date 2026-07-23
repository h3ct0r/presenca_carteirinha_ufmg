#include "app/event_bus.h"

#include "esp32-hal-log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

static const char* TAG = "event_bus";

// 16 pending events is generous: the consumer drains every ~5 ms, and the
// producers (card taps, wifi/battery state changes) are orders of magnitude
// slower. A full queue therefore means the LVGL thread is stuck, and
// dropping is the right call — blocking a service task wouldn't unstick it.
static constexpr UBaseType_t QUEUE_LEN = 16;

static QueueHandle_t s_queue = nullptr;

void event_bus_init(void) {
    s_queue = xQueueCreate(QUEUE_LEN, sizeof(app_event_t));
    configASSERT(s_queue);
}

bool event_bus_post(const app_event_t* ev) {
    if (xQueueSend(s_queue, ev, 0) != pdTRUE) {
        ESP_LOGW(TAG, "queue full, dropped event %u", (unsigned)ev->type);
        return false;
    }
    return true;
}

bool event_bus_poll(app_event_t* ev) {
    return xQueueReceive(s_queue, ev, 0) == pdTRUE;
}
