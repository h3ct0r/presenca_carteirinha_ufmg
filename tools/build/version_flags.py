# PlatformIO pre-script: inject -D APP_GIT_SHA=<short sha> for About / boot log.
# Falls back to "nogit" when git is unavailable (see include/app/version.h).
Import("env")  # type: ignore  # PlatformIO injects Import

import subprocess


def _git_sha():
    try:
        out = subprocess.check_output(
            ["git", "rev-parse", "--short=7", "HEAD"],
            stderr=subprocess.DEVNULL,
        )
        sha = out.decode("ascii", errors="ignore").strip()
        if sha and all(c in "0123456789abcdef" for c in sha.lower()):
            return sha
    except (subprocess.CalledProcessError, FileNotFoundError, OSError):
        pass
    return "nogit"


env.Append(CPPDEFINES=[("APP_GIT_SHA", env.StringifyMacro(_git_sha()))])
