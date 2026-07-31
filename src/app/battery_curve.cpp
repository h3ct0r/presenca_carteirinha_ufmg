#include "app/battery_curve.h"

// mV -> %, high to low. Linear across the pack's usable range
// 4.20 V (100%) .. 2.70 V (0%), with an anchor every 5% (75 mV) so the mapping
// is easy to tweak per-anchor later; battery_mv_to_pct() interpolates linearly
// between adjacent anchors.
//
// These are post-calibration volts, i.e. what battery_service reports after
// BAT_CAL_SCALE — not raw cell volts. Endpoints under load are still unverified;
// see BACKLOG.md §Q4 / BATTERY_DRAIN_LOG in platformio.ini.
struct volt_pct_t {
    uint16_t mv;
    uint8_t pct;
};
static const volt_pct_t CURVE[] = {
    {4200, 100},
    {4125, 95},
    {4050, 90},
    {3975, 85},
    {3900, 80},
    {3825, 75},
    {3750, 70},
    {3675, 65},
    {3600, 60},
    {3525, 55},
    {3450, 50},
    {3375, 45},
    {3300, 40},
    {3225, 35},
    {3150, 30},
    {3075, 25},
    {3000, 20},
    {2925, 15},
    {2850, 10},
    {2775, 5},
    {2700, 0},
};

// No charge-status pin on this board. Treat readings strictly above the 100%
// anchor as "on the charger" — a full pack at 4.20 V is not charging; the rail
// only goes higher while USB is actively topping the cell.
static constexpr uint16_t CHARGING_MV = 4200;

bool battery_is_charging(uint16_t mv) { return mv > CHARGING_MV; }

uint8_t battery_mv_to_pct(uint16_t mv) {
    const int n = sizeof(CURVE) / sizeof(CURVE[0]);
    if (mv >= CURVE[0].mv) return 100;
    if (mv <= CURVE[n - 1].mv) return 0;
    for (int i = 1; i < n; i++) {
        if (mv >= CURVE[i].mv) {
            const volt_pct_t& hi = CURVE[i - 1];
            const volt_pct_t& lo = CURVE[i];
            float f = (float)(mv - lo.mv) / (float)(hi.mv - lo.mv);
            return (uint8_t)(lo.pct + f * (hi.pct - lo.pct) + 0.5f);
        }
    }
    return 0;
}
