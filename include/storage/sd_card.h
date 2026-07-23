#pragma once

// Single owner of the SD_MMC mount. Both config_service (reads config.json)
// and photo_store (writes photos) go through here so the card is mounted
// exactly once, regardless of which runs first. Thread-safe and idempotent.

// Mounts the card if it isn't already. Returns true if mounted. Safe to call
// repeatedly and from any task.
bool sd_card_mount(void);

// Whether the card is currently mounted. Cheap, no locking.
bool sd_card_is_mounted(void);

// Fills the filesystem's total and used byte counts (either pointer may be
// NULL). Returns false if the card isn't mounted. Mounts on demand.
bool sd_card_usage(unsigned long long* total_bytes, unsigned long long* used_bytes);
