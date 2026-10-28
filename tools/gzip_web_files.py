# Gzips the dashboard's files on their way into the device's filesystem image.
#
# data/ stays as it is written — plain HTML, CSS and JS, which is also what
# the page tests read. When PlatformIO builds the LittleFS image (buildfs,
# uploadfs), this script makes a copy of data/ under the build directory with
# every text file replaced by its .gz, and points the image builder at that
# copy instead. The firmware serves x.gz with "Content-Encoding: gzip" when it
# is asked for x (serveWebFile() in src/web/server.cpp), so nothing in the
# pages changes — they just arrive at about a third of the size, over a web
# server that handles one request at a time.
#
# Hooked in from platformio.ini (extra_scripts = pre:...). It has to run
# before the platform's own script, which is where the image builder reads
# PROJECT_DATA_DIR.

Import("env")

import gzip
import json
import os
import shutil

from SCons.Script import COMMAND_LINE_TARGETS

FS_TARGETS = {"buildfs", "uploadfs", "uploadfsota"}

# Anything else (an image, a font) is already compressed and is copied as is.
TEXT_TYPES = {".html", ".css", ".js", ".json", ".svg", ".txt"}


def pack(source_dir, staged_dir):
    """Copies source_dir into staged_dir, gzipping the text files.

    Returns (bytes before, bytes after).
    """
    shutil.rmtree(staged_dir, ignore_errors=True)

    before = after = 0

    for folder, _, names in os.walk(source_dir):
        target_folder = os.path.join(staged_dir, os.path.relpath(folder, source_dir))
        os.makedirs(target_folder, exist_ok=True)

        for name in names:
            source = os.path.join(folder, name)
            before += os.path.getsize(source)

            if os.path.splitext(name)[1].lower() in TEXT_TYPES:
                target = os.path.join(target_folder, name + ".gz")

                # mtime=0 and no file name in the header: the same input
                # always gives the same bytes.
                with open(source, "rb") as plain, open(target, "wb") as raw:
                    with gzip.GzipFile(filename="", mode="wb", fileobj=raw, compresslevel=9, mtime=0) as packed:
                        shutil.copyfileobj(plain, packed)
            else:
                target = os.path.join(target_folder, name)
                shutil.copyfile(source, target)

            after += os.path.getsize(target)

    return before, after


if FS_TARGETS & set(COMMAND_LINE_TARGETS):
    source_dir = env.subst("$PROJECT_DATA_DIR")
    staged_dir = os.path.join(env.subst("$BUILD_DIR"), "data_gz")

    before, after = pack(source_dir, staged_dir)
    print("Web files gzipped for the filesystem image: %.0f KB -> %.0f KB" % (before / 1024, after / 1024))

    # Which code these pages are, for Settings to show next to the firmware's
    # (the values come from version_stamp.py). Only in the image: data/ has
    # no such file.
    # Gzipped like the rest: serveWebFile() looks for the .gz first, and the
    # framework logs an error for every lookup of a file that isn't there.
    stamp = json.dumps({"version": env.get("DIALED_VERSION", "unknown"),
                        "build": env.get("DIALED_BUILD_TIME", "")})
    with open(os.path.join(staged_dir, "version.json.gz"), "wb") as raw:
        with gzip.GzipFile(filename="", mode="wb", fileobj=raw, compresslevel=9, mtime=0) as packed:
            packed.write(stamp.encode("utf-8"))

    env.Replace(PROJECT_DATA_DIR=staged_dir)
