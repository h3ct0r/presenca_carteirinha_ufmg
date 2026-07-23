#pragma once

// Battery monitor: periodically samples the on-board battery-voltage divider
// and publishes APP_EVENT_POWER_STATE (percentage) to the event bus, which
// the status bar renders.
//
// Hardware (schematic sheet 4, "USB&IO"): BAT+ -- R52(68k) --+-- R57(100k)
// -- GND, with the mid node filtered by C61(10nF) and wired to GPIO53 =
// ADC2_CHANNEL4. So V_node = V_BAT * 100/168 and V_BAT = V_node * 1.68.
//
// Requires event_bus_init() first. Starts its own FreeRTOS task; returns
// false only if the task can't be created.
bool battery_service_start(void);
