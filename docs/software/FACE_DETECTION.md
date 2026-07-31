# Camera & Face Detection

The onboard OV02C10 camera is integrated as a **service + screen**, matching the
project's layering (services own hardware and run tasks; screens are the only
code that touches LVGL).

## Modules

| Piece | Files | Role |
| --- | --- | --- |
| Sensor driver | `camera/ov02c10_camera.{h,cpp}` (+ `ov02c10_init_regs.h`, `ov02c10_gain_map.h`) | OV02C10 I²C bring-up, streaming, exposure/gain |
| Capture pipeline | `camera/csi_pipeline.{h,cpp}` | MIPI-CSI RAW10 → ISP demosaic → RGB565 frames |
| Auto exposure/WB | `camera/auto_exposure.{h,cpp}` | Software AE + gray-world AWB from the preview |
| Service | `services/face_detection_service.{h,cpp}` | Owns the pipeline, runs the detection task, publishes a thread-safe preview + face boxes. **No LVGL.** |
| Screen | `ui/screens/scr_camera.{h,cpp}` | Renders the preview + boxes + status, snapshot button. Drives the service via `on_show`/`on_hide`. |
| Model (optional) | `lib/human_face_detect/` | ESP-DL `human_face_detect` (MSR+MNP S8 V1), P4 `.espdl` models |

Reached from **Admin → Camera → Camera preview**. The same service backs the
face-verified kiosk check-in described in [FACE_CHECKIN.md](FACE_CHECKIN.md).

## Data flow

```
OV02C10 ──CSI──▶ ISP ──▶ csi_pipeline_get_frame()  (full-res RGB565, on the detection task)
                              │
                     PPA downscale 1920x1080 → 480x270
                              │
                     AutoExposure.update()  (nudges sensor + ISP)
                              │
                 [USE_FACE_DETECT] HumanFaceDetect.run() → boxes
                              │
        publish under mutex: preview double-buffer + boxes + status
                              │
   scr_camera refresh timer (LVGL thread) ── face_detection_snapshot() ──▶ lv_image + box overlays
```

The service exposes a copy-out API only — `face_detection_start/stop/running`,
`face_detection_snapshot`, `face_detection_status`, `face_detection_model_info`
and `face_detection_request_capture` — so the UI never reaches into camera
buffers and every LVGL call stays on the LVGL thread.

## Real detection is enabled

ESP-DL is vendored (`lib/esp-dl`, an ESP32-P4 subset) and compiled in. The build
wiring lives in `platformio.ini`:

- `-I` include paths mirroring esp-dl's CMake `include_dirs`, plus the prebuilt
  `-lfbs_model` FlatBuffer parser (`lib/esp-dl/fbs_loader/lib/esp32p4`).
- Kconfig-equivalent `-D CONFIG_*` defines (pixel conversions, model selection).
- `-D USE_FACE_DETECT` — compiles the model path in `face_detection_service.cpp`.
- `lib/human_face_detect/library.json` declares `dependencies: { esp-dl }` so the
  Library Dependency Finder actually builds esp-dl's sources (without it you get
  `undefined reference to dl::...` at link time — the headers resolve via `-I`
  but the LDF can't otherwise see the dependency).

The `human_face_detect` model wrapper is still guarded so the camera preview
alone builds if `USE_FACE_DETECT` is removed.

### Models load from the SD card

The two `.espdl` models are loaded at runtime from **`/models/` on the SD card**
(`CONFIG_HUMAN_FACE_DETECT_MODEL_IN_SDCARD`), so there is nothing to pack or
flash:

```
/models/human_face_detect_msr_s8_v1.espdl
/models/human_face_detect_mnp_s8_v1.espdl
```

Copy them from [`sd_card_example/models/`](sd_card_example/models/) (originals in
`lib/human_face_detect/models/p4/`). The file names are hard-coded in
`human_face_detect.cpp`, so keep them exact. The detection task loads the model
on startup (after the SD is mounted); if the files are missing, the status line
shows the load error and no boxes appear.

> The `human_face_det` flash partition in `partitions_model.csv` is now unused
> (SD loading replaces it); it's harmless reserved space. To use flash-partition
> loading instead you'd need esp-dl's `pack_models` tool to build the partition
> image — SD loading avoids that entirely.

## Status / caveats

Working on hardware, including detection.

- **Pixel format is auto-discovered.** `run_detection()` tries RGB565-LE,
  RGB565-BE, then a manual RGB565→RGB888 conversion. Dropping the 888 path in a
  refactor once caused inference to return **0 faces** while everything else
  looked healthy, which took a long time to find — keep all three.
- **Debugging a detection regression.** Open the camera screen, stand in front of
  it, and read the status line (`frame N  faces K [fmt]  infer Xms  luma L`):
  - `faces ≥1 [888]`/`[LE]`/`[BE]` — working; that is the format in use.
  - `faces 0 [none]` — not a format problem. Check whether the preview looks
    right and what `luma` reads (healthy ≈ 60–160; ~0 or ~255 means the exposure
    or pipeline is broken), which points at CSI/ISP/AE rather than detection.
  - Image fine but still `[none]` — look at model input size and normalization.
- **Always-on once opened.** The camera screen starts the detection task and does
  not stop it on exit, so streaming and inference keep running. `face_detection_stop()`
  exists and the kiosk does pause/resume around verification; the camera screen
  could do the same to save battery.
- **Snapshots** go to `/photos/IMG_nnnn.jpg` via `photo_store` (P4 hardware JPEG
  encoder, ~300–500 KB at 1080p; uncompressed `.bmp` only if the JPEG engine is
  unavailable). These are the manual "Take picture" snapshots and are distinct
  from check-in evidence photos — see [SD_CARD.md](SD_CARD.md).
