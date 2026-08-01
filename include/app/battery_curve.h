#pragma once

#include <stdint.h>

// Single-cell Li-ion charge estimate from the anchor table in the .cpp. Pure
// logic (no hardware), so it lives in app/ and is unit-tested natively;
// battery_service feeds it the measured cell voltage. Good enough for a status
// icon, not a fuel gauge.
uint8_t battery_mv_to_pct(uint16_t mv);

// True when the rail reads strictly above the pack's 4.20 V full anchor — this
// board's only charging signal (IP5306 status pins are not wired to the P4). A
// full cell at exactly 4.20 V is not charging; USB topping pushes the reading
// higher.
bool battery_is_charging(uint16_t mv);
