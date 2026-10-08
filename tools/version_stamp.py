# Stamps every build with the code it was made from.
#
# The version is `git describe`: the last tag, the number of commits since it,
# the commit, and "-dirty" when the build included changes that were not
# committed — v0.1.0-3-gabc1234-dirty. With no tag yet it is just the commit.
# Next to it goes the date and time of the build, which is what tells two
# builds of the same uncommitted code apart.
#
# Both are written to include/version_stamp.h (not in the repository), which
# only src/system/version.cpp includes, so a new stamp recompiles that one
# file. They are also left in the build environment for gzip_web_files.py,
# which puts them in the filesystem image — this script has to be listed
# before it in platformio.ini.

Import("env")

import os
import subprocess
import time


def describe(project_dir):
    try:
        result = subprocess.run(
            ["git", "describe", "--tags", "--always", "--dirty"],
            cwd=project_dir, capture_output=True, text=True, timeout=15)
    except (OSError, subprocess.SubprocessError):
        return "unknown"

    # Not a repository (a source archive), or git is not installed.
    return result.stdout.strip() if result.returncode == 0 and result.stdout.strip() else "unknown"


project_dir = env.subst("$PROJECT_DIR")

version = describe(project_dir)
built = time.strftime("%Y-%m-%d %H:%M:%S")

env["DIALED_VERSION"] = version
env["DIALED_BUILD_TIME"] = built

with open(os.path.join(env.subst("$PROJECT_INCLUDE_DIR"), "version_stamp.h"), "w", newline="\n") as header:
    header.write("// Written by tools/version_stamp.py on every build. Not in the repository.\n")
    header.write('#define FIRMWARE_VERSION_STAMP "%s"\n' % version)
    header.write('#define FIRMWARE_BUILD_STAMP "%s"\n' % built)

print("Version stamp: %s, built %s" % (version, built))
