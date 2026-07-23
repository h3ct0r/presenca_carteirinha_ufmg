#include "app/battery_curve.h"

// mV -> %, high to low. Linear from 4.2 V (100%) to 3.2 V (0%), with an anchor
// every 5% (each 50 mV) so the mapping is easy to tweak per-anchor later;
// battery_mv_to_pct() interpolates linearly between adjacent anchors.
struct volt_pct_t {
    uint16_t mv;
    uint8_t pct;
};
static const volt_pct_t CURVE[] = {
    {4000, 100},
    {3935, 95},
    {3870, 90},
    {3805, 85},
    {3740, 80},
    {3675, 75},
    {3610, 70},
    {3545, 65},
    {3480, 60},
    {3415, 55},
    {3350, 50},
    {3285, 45},
    {3220, 40},
    {3155, 35},
    {3090, 30},
    {3025, 25},
    {2960, 20},
    {2895, 15},
    {2830, 10},
    {2765, 5},
    {2700, 0},
};

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
