#ifndef _CSI_PIPELINE_H
#define _CSI_PIPELINE_H
#include <stdint.h>
#include <stddef.h>

// MIPI-CSI capture pipeline: OV02C10 RAW10 (2-lane) -> ISP demosaic -> RGB565 frames.
bool csi_pipeline_init(uint16_t w, uint16_t h);

// Blocks until a full RGB565 frame arrives (or timeout). Returns the frame
// buffer, or nullptr on timeout. Buffer stays valid until the next call.
uint8_t* csi_pipeline_get_frame(uint32_t timeout_ms, size_t* out_len);

// White balance: scales the R and B columns of the color correction matrix.
// Called by the software AWB loop; gains are relative to green (1.0 = neutral).
void csi_pipeline_set_wb(float r_gain, float b_gain);

#endif
