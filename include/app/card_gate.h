#pragma once

#include <stdint.h>

// Decides what a stream of RFID poll results means. Pure logic, no hardware, so
// the reader task stays thin and this is covered by the native tests.
//
// The problem it solves: the PN532 is asked for ONE target per poll
// (`InListPassiveTarget` with MaxTg=1), and with two cards in the field the one
// that wins anticollision **alternates between polls**. A debounce that only
// remembers the previous UID therefore never fires — every poll looks like a
// new card, and both students get checked in over and over at the poll rate.
//
// Two rules fix that:
//
//  1. **Confirm before accepting.** A UID must come back on
//     CARD_GATE_CONFIRM_POLLS consecutive polls before it is reported. Two
//     cards alternating never produce a run that long, so neither is accepted.
//     One card costs one extra poll (~100 ms) before it registers.
//
//  2. **Alternation means two cards.** If a UID already seen since the field
//     last went empty comes back after a different one (A → B → A), more than
//     one card is present. That is reported once so the UI can say so, instead
//     of the device looking dead.
//
// A single card lifted and re-presented is always accepted again: an empty
// field resets everything.

// Consecutive identical polls required before a card is accepted. Two is enough
// to break alternation and keeps the added latency to one poll interval.
static constexpr int CARD_GATE_CONFIRM_POLLS = 2;

typedef enum {
    CARD_GATE_IGNORE = 0,  // nothing to do: no card, still confirming, or a duplicate
    CARD_GATE_ACCEPT,      // a single card is confirmed — register this tap
    CARD_GATE_COLLISION,   // several cards on the reader — reported once per presentation
} card_gate_action_t;

// Feeds one poll result. Pass uid_len == 0 (or uid == NULL) when the field is
// empty. `out_uid` (optional, >= 10 bytes) receives the accepted UID on
// CARD_GATE_ACCEPT.
card_gate_action_t card_gate_poll(const uint8_t* uid, uint8_t uid_len, uint8_t* out_uid,
                                  uint8_t* out_uid_len);

// Forgets everything, as if the reader had just started.
void card_gate_reset(void);
