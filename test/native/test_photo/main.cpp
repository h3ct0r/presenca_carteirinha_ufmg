// photo_store against the mock SD card, mock JPEG engine, and pthread-backed
// FreeRTOS: "taking a picture" is a synthetic RGB565 frame pushed through
// photo_store_capture(); the writer task then runs for real (as a thread)
// and the tests assert on the files written to the in-memory card.
//
// Ordering matters twice: the mount-failure test must be first (mount
// latches), and the BMP-fallback scenario runs before the JPEG one because
// the photo index is a monotonic static.

#include <string.h>
#include <unistd.h>
#include <unity.h>

#include "mock_freertos.h"
#include "mock_jpeg.h"
#include "mock_sd.h"
#include "storage/photo_store.h"

void setUp(void) {}
void tearDown(void) {}

static uint16_t s_frame[8 * 8];  // tiny synthetic camera frame

// The SD write happens on the writer task; poll for the file.
static bool wait_for_file(const char* path, int timeout_ms) {
    for (int waited = 0; waited < timeout_ms; waited += 10) {
        if (mocksd_exists(path)) {
            usleep(20 * 1000);  // let the status line settle after the write
            return true;
        }
        usleep(10 * 1000);
    }
    return false;
}

static void test_init_fails_without_card(void) {
    mocksd_reset();
    mocksd_set_begin_result(false);
    TEST_ASSERT_FALSE(photo_store_init());
    char status[48];
    photo_store_get_status(status, sizeof(status));
    TEST_ASSERT_EQUAL_STRING("no SD card", status);
}

static void test_capture_rejected_before_mount(void) {
    TEST_ASSERT_FALSE(photo_store_capture((const uint8_t*)s_frame, 8, 8));
}

static void test_bmp_fallback_when_encoder_unavailable(void) {
    mocksd_reset();
    mock_jpeg_set_engine_available(false);
    TEST_ASSERT_TRUE(photo_store_init());

    memset(s_frame, 0x5A, sizeof(s_frame));
    TEST_ASSERT_TRUE(photo_store_capture((const uint8_t*)s_frame, 8, 8));
    TEST_ASSERT_TRUE(wait_for_file("/photos/IMG_0001.bmp", 2000));

    // 54-byte BMP header + 8 rows of 8 px * 3 bytes (24 = already 4-aligned).
    TEST_ASSERT_EQUAL_size_t(54 + 24 * 8, mocksd_file_size("/photos/IMG_0001.bmp"));
    uint8_t hdr[54];
    TEST_ASSERT_EQUAL_size_t(54, mocksd_read_file("/photos/IMG_0001.bmp", hdr, sizeof(hdr)));
    TEST_ASSERT_EQUAL_UINT8('B', hdr[0]);
    TEST_ASSERT_EQUAL_UINT8('M', hdr[1]);
    TEST_ASSERT_EQUAL_UINT8(8, hdr[18]);  // width, little-endian
    TEST_ASSERT_EQUAL_UINT8(8, hdr[22]);  // height
    TEST_ASSERT_EQUAL_UINT8(24, hdr[28]);  // bpp

    char status[48];
    photo_store_get_status(status, sizeof(status));
    TEST_ASSERT_EQUAL_STRING("saved IMG_0001.bmp", status);
}

static void test_jpeg_path_and_numbering_continues(void) {
    mocksd_reset();
    mock_jpeg_set_engine_available(true);
    // Photos already on the card: numbering must continue after them.
    mocksd_add_file("/photos/IMG_0041.jpg", "existing");
    TEST_ASSERT_TRUE(photo_store_init());

    uint32_t seq_before = photo_store_last_saved(nullptr, 0);
    TEST_ASSERT_TRUE(photo_store_capture((const uint8_t*)s_frame, 8, 8));
    TEST_ASSERT_TRUE(wait_for_file("/photos/IMG_0042.jpg", 2000));

    uint8_t head[9];
    TEST_ASSERT_EQUAL_size_t(9, mocksd_read_file("/photos/IMG_0042.jpg", head, sizeof(head)));
    TEST_ASSERT_EQUAL_UINT8(0xFF, head[0]);  // fake-JPEG SOI from the mock
    TEST_ASSERT_EQUAL_UINT8(0xD8, head[1]);
    TEST_ASSERT_EQUAL_UINT8_ARRAY((const uint8_t*)"MOCKJPG", head + 2, 7);

    // The save bumps the sequence and records the full path (what the camera
    // screen polls to show its "picture saved" popup).
    char path[64];
    uint32_t seq_after = photo_store_last_saved(path, sizeof(path));
    TEST_ASSERT_EQUAL_UINT32(seq_before + 1, seq_after);
    TEST_ASSERT_EQUAL_STRING("/photos/IMG_0042.jpg", path);
}

static void test_capture_rejects_null_frame(void) {
    TEST_ASSERT_FALSE(photo_store_capture(nullptr, 8, 8));
}

static void test_capture_rejects_degenerate_dimensions(void) {
    TEST_ASSERT_FALSE(photo_store_capture((const uint8_t*)s_frame, 0, 8));
    TEST_ASSERT_FALSE(photo_store_capture((const uint8_t*)s_frame, 8, -1));
}

// The frame buffers used to be sized by the FIRST capture and then memcpy'd at
// the caller's size forever after — a heap overflow the moment a larger frame
// arrived, held off only by the camera resolution being a compile-time
// constant. A bigger frame after a smaller one is the case that used to smash
// the heap; run it under whatever hardening the host build has.
static uint16_t s_big_frame[16 * 16];

static void test_capture_resizes_buffers_between_frames(void) {
    for (int i = 0; i < 16 * 16; i++) s_big_frame[i] = (uint16_t)(0xA000 | i);

    // Small -> large: the buffer must grow, not be written past.
    TEST_ASSERT_TRUE(photo_store_capture((const uint8_t*)s_frame, 8, 8));
    TEST_ASSERT_TRUE(wait_for_file("/photos/IMG_0043.jpg", 2000));
    TEST_ASSERT_TRUE(photo_store_capture((const uint8_t*)s_big_frame, 16, 16));
    TEST_ASSERT_TRUE(wait_for_file("/photos/IMG_0044.jpg", 2000));

    // And back down again, so the shrink path is exercised too.
    TEST_ASSERT_TRUE(photo_store_capture((const uint8_t*)s_frame, 8, 8));
    TEST_ASSERT_TRUE(wait_for_file("/photos/IMG_0045.jpg", 2000));
}

// photo_store is initialized (mounted + JPEG engine) from the JPEG test above.
static void test_encode_to_writes_given_path(void) {
    const char* path = "/students/checkins/2023-0142/2026-07-28_CS101-M1_01.jpg";
    TEST_ASSERT_TRUE(photo_store_encode_to(path, (const uint8_t*)s_frame, 8, 8));
    uint8_t head[9];
    TEST_ASSERT_EQUAL_size_t(9, mocksd_read_file(path, head, sizeof(head)));
    TEST_ASSERT_EQUAL_UINT8(0xFF, head[0]);  // fake-JPEG SOI from the mock
    TEST_ASSERT_EQUAL_UINT8(0xD8, head[1]);
    TEST_ASSERT_FALSE(photo_store_encode_to(nullptr, (const uint8_t*)s_frame, 8, 8));
}

int main(int, char**) {
    mock_freertos_set_delay_scale(1000);
    UNITY_BEGIN();
    RUN_TEST(test_init_fails_without_card);  // must stay first (mount latches)
    RUN_TEST(test_capture_rejected_before_mount);
    RUN_TEST(test_bmp_fallback_when_encoder_unavailable);
    RUN_TEST(test_jpeg_path_and_numbering_continues);
    RUN_TEST(test_encode_to_writes_given_path);  // after init (mounted + engine)
    RUN_TEST(test_capture_rejects_null_frame);
    RUN_TEST(test_capture_rejects_degenerate_dimensions);
    RUN_TEST(test_capture_resizes_buffers_between_frames);
    return UNITY_END();
}
