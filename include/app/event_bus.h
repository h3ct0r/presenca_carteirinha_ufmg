#pragma once

#include "app/app_events.h"

// Single FreeRTOS queue connecting the service tasks (producers) to the LVGL
// thread (the only consumer). This is the one place where cross-task traffic
// happens, so services never need to know about LVGL's threading rules.

// Creates the queue. Call once in setup(), before any service starts.
void event_bus_init(void);

// Posts an event from any task. Non-blocking: if the queue is full the event
// is dropped (and logged) rather than stalling the producer. Not ISR-safe.
bool event_bus_post(const app_event_t* ev);

// Fetches the next pending event, non-blocking. Call only from the LVGL
// thread, in a drain loop right before lv_timer_handler().
bool event_bus_poll(app_event_t* ev);
