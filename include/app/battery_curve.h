#pragma once

#include <stdint.h>

// Single-cell Li-ion charge estimate from the anchor table in the .cpp. Pure
// logic (no hardware), so it lives in app/ and is unit-tested natively;
// battery_service feeds it the measured cell voltage. Good enough for a status
// icon, not a fuel gauge.
uint8_t battery_mv_to_pct(uint16_t mv);

// True when the rail reads higher than the pack alone can drive it, which is
// this board's only charging signal — the IP5306's status pins are not wired to
// the P4. The curve's 100% anchor is well below the threshold, so a merely full
// battery never trips it; being fed by USB does.
bool battery_is_charging(uint16_t mv);
