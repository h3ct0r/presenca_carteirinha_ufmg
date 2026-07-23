#pragma once

// Whether jpeg_new_encoder_engine() succeeds (default true). Set false to
// exercise photo_store's BMP fallback path.
void mock_jpeg_set_engine_available(bool available);
