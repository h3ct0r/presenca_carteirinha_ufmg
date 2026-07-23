#pragma once

#include <stdint.h>

// Fake PN532 reader implementing rfid/pn532_reader.h's contract: instead of
// polling hardware, tests "present" cards with mock_pn532_tap(), which
// invokes the registered callback synchronously (like the real reader task
// would from its own context).
void mock_pn532_set_start_result(bool ok);
bool mock_pn532_started(void);
void mock_pn532_tap(const uint8_t* uid, uint8_t uid_len);
