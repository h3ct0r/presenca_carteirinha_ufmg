// Build identity shown in the header, so a bug report pins which copy of the
// tool produced a config.tar — the same job include/app/version.h does for the
// firmware, and worth as much here: this tool is rsync'd to a server (see
// DEPLOY.md), so a stale deployment is easy to miss and hard to spot.

// Mirrors APP_VERSION in include/app/version.h. Hand-kept — there is no build
// step to inject it — but NOT left to vigilance: test/schema-sync.test.js reads
// the firmware header and fails if the two drift, the same guard that protects
// the LIMITS table.
export const APP_VERSION = 'v0.3.0';

// Short git SHA of the deployed tree, or '' when unknown.
//
// Deliberately empty in the repo. The tool is served as static files with no
// build step, so nothing can fill this in automatically at load time; a deploy
// that wants the hash stamps it with the one-liner in DEPLOY.md ("Stamping the
// build hash"). Showing an honest 'v0.2.0' beats showing a hash that is really
// "whatever was checked out the last time someone edited this line".
export const BUILD_SHA = '';

// "v0.2.0+89f20c1" when stamped, "v0.2.0" otherwise. Matches the firmware's
// APP_VERSION_FULL format so the two can be compared at a glance.
export function versionLabel() {
  return BUILD_SHA ? `${APP_VERSION}+${BUILD_SHA}` : APP_VERSION;
}
