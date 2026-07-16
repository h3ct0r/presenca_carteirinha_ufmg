#pragma once

// Short confirmation beep through the board's audio path: ES8311 codec
// (shared I2C bus) -> I2S -> NS4150 class-D amp -> CN1 speaker connector
// (passive electromagnetic buzzer or small speaker).
//
// beeper_init: call once from setup() after the I2C bus is up. Returns
// false if the codec doesn't answer; beeper_beep is then a no-op.
bool beeper_init();

// Queues one beep. Non-blocking, safe to call from any task (not ISR).
void beeper_beep();
