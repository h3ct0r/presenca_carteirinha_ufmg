#pragma once

// Short confirmation beep through the board's audio path: ES8311 codec
// (shared I2C bus) -> I2S -> NS4150 class-D amp -> CN1 speaker connector
// (passive electromagnetic buzzer or small speaker).
//
// beeper_init: call once from setup() after the I2C bus is up. Returns
// false if the codec doesn't answer; the beep calls are then no-ops.
bool beeper_init();

// All three queue one sound and return immediately. Non-blocking, safe from
// any task (not ISR).

// General confirmation beep (e.g. a card was read).
void beeper_beep();

// Short, quiet tick for UI taps (button/card presses).
void beeper_touch();

// Distinct low double-beep signalling a problem / access denied.
void beeper_error();
