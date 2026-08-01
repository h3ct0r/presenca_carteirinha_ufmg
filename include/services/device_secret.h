#pragma once

#include <stddef.h>
#include <stdint.h>

// The per-device key behind every stored credential fingerprint
// (app/credential.h). 32 random bytes, generated once on first boot and kept in
// NVS — on the internal flash, NOT on the SD card. That separation is the whole
// security argument: an attacker who takes the card, or who reads it through the
// unauthenticated debug file manager, gets fingerprints with no key to test
// guesses against.
//
// Consequences, which SD_CARD.md also records:
//  - Fingerprints are DEVICE-BOUND. Moving a card to a second device makes every
//    card binding and password on it unrecognisable there.
//  - Erasing NVS (a full flash erase) has the same effect on this device. The
//    recovery path is Admin -> debug -> "Delete all cards & attendance" followed
//    by re-enrolment, and re-importing config.tar for the passwords.
//  - It is stable across ordinary firmware updates: NVS survives an app-only
//    flash, which is why the firmware version would have been a terrible key.
//
// Never log it, never show it, never write it to the card.

constexpr size_t DEVICE_SECRET_LEN = 32;

// Fills `out` with the device key, generating and persisting one on first call.
// Returns false only if NVS is unavailable, in which case `out` is zeroed and
// the caller must fail closed rather than fingerprint under an all-zero key.
bool device_secret_get(uint8_t out[DEVICE_SECRET_LEN]);
