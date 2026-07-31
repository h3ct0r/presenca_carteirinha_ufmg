// photo_fit: aspect-preserving scale/size math behind the avatar widgets.
// 256 == 1:1 (LVGL's LV_SCALE_NONE), and the box is a FIT, not just a cap —
// a smaller source is enlarged to meet it.

#include <unity.h>

#include "app/photo_fit.h"

void setUp(void) {}
void tearDown(void) {}

static const int BOX = 250;  // the avatar box the screens use

// Convenience: the drawn size for a source, as a pair.
static void fit(int w, int h, int max_px, int* ow, int* oh) {
    photo_fit_size(w, h, max_px, ow, oh);
}

static void test_square_smaller_than_box_is_enlarged(void) {
    // The authored 100x100 avatar: 2.5x up to fill the box.
    TEST_ASSERT_EQUAL_INT(640, photo_fit_scale(100, 100, BOX));
    int w, h;
    fit(100, 100, BOX, &w, &h);
    TEST_ASSERT_EQUAL_INT(250, w);
    TEST_ASSERT_EQUAL_INT(250, h);
}

static void test_square_exactly_the_box_is_untouched(void) {
    TEST_ASSERT_EQUAL_INT(PHOTO_SCALE_UNIT, photo_fit_scale(250, 250, BOX));
    int w, h;
    fit(250, 250, BOX, &w, &h);
    TEST_ASSERT_EQUAL_INT(250, w);
    TEST_ASSERT_EQUAL_INT(250, h);
}

static void test_square_larger_than_box_is_shrunk(void) {
    TEST_ASSERT_EQUAL_INT(128, photo_fit_scale(500, 500, BOX));  // half
    int w, h;
    fit(500, 500, BOX, &w, &h);
    TEST_ASSERT_EQUAL_INT(250, w);
    TEST_ASSERT_EQUAL_INT(250, h);
}

// The long edge meets the box; the short edge keeps the ratio.
static void test_landscape_keeps_aspect(void) {
    int w, h;
    fit(400, 300, BOX, &w, &h);
    TEST_ASSERT_EQUAL_INT(250, w);
    TEST_ASSERT_EQUAL_INT(188, h);  // 300 * (250/400) = 187.5, rounded
}

static void test_portrait_keeps_aspect(void) {
    int w, h;
    fit(300, 400, BOX, &w, &h);
    TEST_ASSERT_EQUAL_INT(188, w);
    TEST_ASSERT_EQUAL_INT(250, h);
}

// 4:3 and 3:4 are mirror images — the ratio must survive both orientations.
static void test_aspect_ratio_is_orientation_symmetric(void) {
    int lw, lh, pw, ph;
    fit(400, 300, BOX, &lw, &lh);
    fit(300, 400, BOX, &pw, &ph);
    TEST_ASSERT_EQUAL_INT(lw, ph);
    TEST_ASSERT_EQUAL_INT(lh, pw);
}

// A banner-shaped source must not round its short edge away to nothing — a
// zero-sized widget vanishes from the flex layout instead of showing a sliver.
static void test_extreme_ratio_keeps_a_visible_short_edge(void) {
    int w, h;
    fit(1000, 10, BOX, &w, &h);
    TEST_ASSERT_EQUAL_INT(250, w);
    TEST_ASSERT_TRUE(h >= 1);

    fit(4000, 1, BOX, &w, &h);
    TEST_ASSERT_EQUAL_INT(250, w);
    TEST_ASSERT_EQUAL_INT(1, h);  // clamped up, never 0
}

// The invariant the callers rely on: whatever comes in, neither axis of the
// widget exceeds the box it was given.
static void test_never_exceeds_the_box(void) {
    const int sizes[] = {1, 2, 7, 33, 99, 100, 128, 249, 250, 251, 640, 1024, 4000};
    const int n = (int)(sizeof(sizes) / sizeof(sizes[0]));
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            int w, h;
            fit(sizes[i], sizes[j], BOX, &w, &h);
            TEST_ASSERT_TRUE_MESSAGE(w >= 1 && w <= BOX, "width outside 1..BOX");
            TEST_ASSERT_TRUE_MESSAGE(h >= 1 && h <= BOX, "height outside 1..BOX");
            // The fit touches the box on at least one axis (within rounding).
            TEST_ASSERT_TRUE_MESSAGE(w >= BOX - 2 || h >= BOX - 2,
                                     "neither axis reached the box");
        }
    }
}

// The face-verify modal reuses the same math with a 64 px box.
static void test_small_box_is_honored(void) {
    int w, h;
    fit(100, 100, 64, &w, &h);
    TEST_ASSERT_EQUAL_INT(64, w);
    TEST_ASSERT_EQUAL_INT(64, h);
    TEST_ASSERT_EQUAL_INT(164, photo_fit_scale(100, 100, 64));  // 64*256/100, rounded
}

// A JPEG whose header failed to parse reports 0x0. Draw it 1:1 and report no
// size rather than dividing by zero or handing back a negative widget size.
static void test_degenerate_inputs_are_safe(void) {
    TEST_ASSERT_EQUAL_INT(PHOTO_SCALE_UNIT, photo_fit_scale(0, 0, BOX));
    TEST_ASSERT_EQUAL_INT(PHOTO_SCALE_UNIT, photo_fit_scale(100, 0, BOX));
    TEST_ASSERT_EQUAL_INT(PHOTO_SCALE_UNIT, photo_fit_scale(-5, 100, BOX));
    TEST_ASSERT_EQUAL_INT(PHOTO_SCALE_UNIT, photo_fit_scale(100, 100, 0));
    TEST_ASSERT_EQUAL_INT(PHOTO_SCALE_UNIT, photo_fit_scale(100, 100, -1));

    int w = -1, h = -1;
    fit(0, 0, BOX, &w, &h);
    TEST_ASSERT_EQUAL_INT(0, w);
    TEST_ASSERT_EQUAL_INT(0, h);
    fit(100, 100, 0, &w, &h);
    TEST_ASSERT_EQUAL_INT(0, w);
    TEST_ASSERT_EQUAL_INT(0, h);
}

static void test_null_out_pointers_are_allowed(void) {
    int w = 0;
    photo_fit_size(100, 100, BOX, &w, nullptr);
    TEST_ASSERT_EQUAL_INT(250, w);
    photo_fit_size(100, 100, BOX, nullptr, nullptr);  // must not crash
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_square_smaller_than_box_is_enlarged);
    RUN_TEST(test_square_exactly_the_box_is_untouched);
    RUN_TEST(test_square_larger_than_box_is_shrunk);
    RUN_TEST(test_landscape_keeps_aspect);
    RUN_TEST(test_portrait_keeps_aspect);
    RUN_TEST(test_aspect_ratio_is_orientation_symmetric);
    RUN_TEST(test_extreme_ratio_keeps_a_visible_short_edge);
    RUN_TEST(test_never_exceeds_the_box);
    RUN_TEST(test_small_box_is_honored);
    RUN_TEST(test_degenerate_inputs_are_safe);
    RUN_TEST(test_null_out_pointers_are_allowed);
    return UNITY_END();
}
