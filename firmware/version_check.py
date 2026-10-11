# Keeps PROJECT_VER (git describe, set in CMakeLists.txt at configure time)
# current. PlatformIO only reconfigures CMake when CMakeLists.txt or sdkconfig
# change, so an incremental build after a commit kept reporting the old hash.
# When git describe differs from the version the build was configured with,
# age CMakeCache.txt so the espidf builder reconfigures.
# GPL-3.0-or-later.
import json
import os
import subprocess

Import("env")  # noqa: F821 (SCons)

build_dir = env.subst("$BUILD_DIR")  # noqa: F821
desc = os.path.join(build_dir, "project_description.json")
cache = os.path.join(build_dir, "CMakeCache.txt")
try:
    want = subprocess.run(["git", "describe", "--tags", "--always", "--dirty"],
                          cwd=env.subst("$PROJECT_DIR"), capture_output=True,  # noqa: F821
                          text=True).stdout.strip() or "unknown"
    with open(desc) as fp:
        have = json.load(fp).get("project_version")
except (OSError, ValueError):
    have = want = None  # no build yet (a fresh configure follows) or no git
if have != want and os.path.isfile(cache):
    print("version_check: %s -> %s, reconfiguring" % (have, want))
    os.utime(cache, (0, 0))
