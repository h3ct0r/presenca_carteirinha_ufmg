#pragma once

// Multiplies every vTaskDelay by this factor. Test suites that start
// services with retry loops set a large scale so the background retry
// effectively never fires during the test run.
void mock_freertos_set_delay_scale(unsigned scale);
