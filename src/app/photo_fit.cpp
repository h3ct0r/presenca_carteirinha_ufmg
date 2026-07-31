#include "app/photo_fit.h"

// All arithmetic goes through long long: the intermediate `max_px * UNIT` is
// large when the source is tiny (a 1 px source in a 250 box scales by 64000),
// and a truncating int multiply there would silently wrap.

static bool usable(int src_w, int src_h, int max_px) {
    return src_w > 0 && src_h > 0 && max_px > 0;
}

// Rounds a/b to nearest instead of truncating. Both are positive here.
static long long div_round(long long a, long long b) { return (a + b / 2) / b; }

int photo_fit_scale(int src_w, int src_h, int max_px) {
    if (!usable(src_w, src_h, max_px)) return PHOTO_SCALE_UNIT;
    const long long longest = src_w > src_h ? src_w : src_h;
    // Rounded, not truncated: truncating loses up to a whole pixel on the long
    // edge (a 100px source in a 64px box came out 63), so the fit would quietly
    // undershoot the box it was asked to fill.
    long long scale = div_round((long long)max_px * PHOTO_SCALE_UNIT, longest);
    // A source longer than max_px * UNIT would round to 0 and make the image
    // vanish; keep the smallest scale that still draws something.
    if (scale < 1) scale = 1;
    return (int)scale;
}

void photo_fit_size(int src_w, int src_h, int max_px, int* out_w, int* out_h) {
    int w = 0, h = 0;
    if (usable(src_w, src_h, max_px)) {
        const long long scale = photo_fit_scale(src_w, src_h, max_px);
        w = (int)div_round((long long)src_w * scale, PHOTO_SCALE_UNIT);
        h = (int)div_round((long long)src_h * scale, PHOTO_SCALE_UNIT);
        // A very wide/tall source rounds its short edge toward 0; a zero-sized
        // widget would disappear from the layout entirely.
        if (w < 1) w = 1;
        if (h < 1) h = 1;
        // Rounding must never push an axis past the box the caller asked for.
        if (w > max_px) w = max_px;
        if (h > max_px) h = max_px;
    }
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
}
