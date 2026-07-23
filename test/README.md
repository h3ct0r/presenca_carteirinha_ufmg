# Tests

Host-side (native) unit tests: the application logic compiles and runs on
your development machine, with every piece of hardware behind a mock. No
device, no SD card, no wiring needed.

## Running

```sh
pio test -e native              # run all suites
pio test -e native -v           # verbose (per-assert output)
pio test -e native -f "native/test_roster"   # one suite
```

Requires PlatformIO with the `native` platform (auto-installed on first run)
and a host C++ toolchain.

`pio run` / device flashing is unaffected: the mock library is excluded from
the `esp32p4` env (`lib_ignore = hw_mocks`, and the lib is marked
native-only).

## Suites

| Suite                    | Covers                                                                 |
| ------------------------ | ---------------------------------------------------------------------- |
| `native/test_core`       | `uid_normalize`, login session, battery voltage→percent curve          |
| `native/test_event_bus`  | event bus FIFO semantics and full-queue drop policy                    |
| `native/test_rfid`       | RFID service: fake card taps → `CARD_SCANNED` events, UID clamping     |
| `native/test_auth_config`| `config.json` parsing (teachers/password), UID→teacher lookup, statuses|
| `native/test_roster`     | SD data layout validation (`students.json`, `classes/*/class.json`) and the exact on-screen error messages |
| `native/test_photo`      | photo capture pipeline: JPEG write, BMP fallback, file numbering       |

## How the hardware is mocked

All mocks live in `lib/hw_mocks/` and substitute the ESP-IDF / Arduino
headers at compile time — production sources are compiled **unmodified**:

- **SD card** — `FS.h` / `SD_MMC.h` back onto an in-memory filesystem.
  Tests seed card contents with `mocksd_add_file("/config.json", "...")`,
  simulate a missing/unreadable card with `mocksd_set_begin_result(false)`,
  and inspect files the code wrote with `mocksd_read_file()`.
- **RFID reader** — `mock_pn532_reader.cpp` implements
  `rfid/pn532_reader.h`; `mock_pn532_tap(uid, len)` "presents a card" by
  firing the same callback the real reader task uses.
- **Camera / photo** — a synthetic RGB565 frame is pushed through
  `photo_store_capture()`; the mock `driver/jpeg_encode.h` "encoder" emits a
  recognizable fake JPEG, and the test asserts on the bytes written to the
  mock SD (including the BMP fallback's real header).
- **FreeRTOS** — queues/mutexes/tasks are pthread-backed
  (`mock_freertos.cpp`), so service background tasks genuinely run as
  threads. `mock_freertos_set_delay_scale(n)` stretches `vTaskDelay`, which
  suites use to park the services' retry loops during a test run.

## Conventions / gotchas

- Each suite is one binary (`test/native/test_<name>/main.cpp`) with its own
  `main()` running Unity. Static state persists **within** a suite, so a few
  tests are order-dependent and say so in a comment — notably the
  "no SD card" scenarios must run first because `sd_card_mount()` latches.
- Tests assert through public APIs only (service status/getters, events,
  files on the mock card) — no reaching into internals.
- What's deliberately *not* covered natively: LVGL screens/UI, display/touch
  drivers, the PN532/I2C/I2S driver glue, and `main.cpp` wiring. Those need
  the device; on-target smoke tests can be added later under
  `test/embedded/` (the `esp32p4` env is already filtered to it).

## Adding a test

1. New case in an existing suite: add a `static void test_x(void)` and a
   `RUN_TEST(test_x)` line.
2. New suite: create `test/native/test_<name>/main.cpp` with `setUp`,
   `tearDown`, and `main()`; it is picked up automatically.
3. New production code: if it touches new hardware APIs, add the smallest
   possible mock header to `lib/hw_mocks/include/` and (if needed) a control
   function in a `mock_*.h`.

## 3D CAD details to check

- RFID can be ofsset a bit to the back, check the screw pilars for this
- different usb cable connector position to help re-route the long cable? 