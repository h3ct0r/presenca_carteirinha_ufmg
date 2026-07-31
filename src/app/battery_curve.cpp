#include "app/battery_curve.h"

// mV -> %, high to low. Linear from 4.2 V (100%) to 3.2 V (0%), with an anchor
// every 5% (each 50 mV) so the mapping is easy to tweak per-anchor later;
// battery_mv_to_pct() interpolates linearly between adjacent anchors.
struct volt_pct_t {
    uint16_t mv;
    uint8_t pct;
};
static const volt_pct_t CURVE[] = {
    {3750, 100},
    {3698, 95},
    {3645, 90},
    {3593, 85},
    {3540, 80},
    {3488, 75},
    {3435, 70},
    {3383, 65},
    {3330, 60},
    {3278, 55},
    {3225, 50},
    {3173, 45},
    {3120, 40},
    {3068, 35},
    {3015, 30},
    {2963, 25},
    {2910, 20},
    {2858, 15},
    {2805, 10},
    {2753, 5},
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
