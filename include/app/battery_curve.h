#pragma once

#include <stdint.h>

// Single-cell Li-ion charge estimate, linear from 4.2 V (100%) to 3.2 V (0%)
// with 5% anchors. Pure logic (no hardware), so it lives in app/ and is
// unit-tested natively; battery_service feeds it the measured cell voltage.
// Good enough for a status icon, not a fuel gauge.
uint8_t battery_mv_to_pct(uint16_t mv);
