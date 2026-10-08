# Uploading the firmware uploads the dashboard too.
#
# The two are separate uploads, and a device with the firmware of one commit
# and the pages of another is a problem waiting to be found on a trail. So
# "upload" always does both, whether or not the pages changed; "uploadfs" on
# its own still exists for a change that only touched data/.
#
# Listed as a post: script in platformio.ini — the "upload" target has to
# exist before something can be attached to it.

Import("env")

import time
import urllib.request

# How long the device gets to restart on the new firmware and rejoin the
# network before the dashboard upload is given up on.
RESTART_TIMEOUT_S = 90


def wait_for_device(address):
    # The device answers the upload, and only then restarts: give it time to
    # go away before looking for it to come back.
    time.sleep(3)

    deadline = time.time() + RESTART_TIMEOUT_S

    while time.time() < deadline:
        try:
            with urllib.request.urlopen("http://%s/api/status" % address, timeout=3):
                return True
        except OSError:
            time.sleep(2)

    return False


def upload_dashboard(source, target, env):
    # Over USB the board is reset by the upload tool and is ready for the
    # next one straight away; over the air it has to be back on the network.
    if env.subst("$UPLOAD_PROTOCOL") == "espota":
        address = env.subst("$UPLOAD_PORT")
        print("Firmware uploaded. Waiting for %s to restart..." % address)

        if not wait_for_device(address):
            print("The device did not come back: the dashboard was NOT uploaded.")
            print("Upload it once the device is reachable: pio run -t uploadfs")
            env.Exit(1)

    print("Uploading the dashboard...")

    if env.Execute('"$PYTHONEXE" -m platformio run -d "$PROJECT_DIR" -e $PIOENV -t uploadfs'):
        print("The firmware is on the device, but the dashboard upload failed.")
        env.Exit(1)


env.AddPostAction("upload", upload_dashboard)
