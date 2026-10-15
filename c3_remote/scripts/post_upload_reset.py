Import("env")

import os
import subprocess
import time


def reset_with_watchdog(source, target, env):
    """Boot the C3 app without relying on the USB control-line reset."""
    platform = env.PioPlatform()
    esptool = os.path.join(
        platform.get_package_dir("tool-esptoolpy"), "esptool.py"
    )
    command = [
        env.subst("$PYTHONEXE"),
        esptool,
        "--chip",
        env.BoardConfig().get("build.mcu", "esp32c3"),
        "--port",
        env.subst("$UPLOAD_PORT"),
        "--after",
        "watchdog_reset",
        "run",
    ]
    time.sleep(0.3)
    result = subprocess.run(
        command, capture_output=True, text=True, errors="replace"
    )
    combined = (result.stdout or "") + (result.stderr or "")
    if "Hard resetting with a watchdog" not in combined:
        print(combined)
        raise RuntimeError("C3 watchdog reset was not sent after upload")
    # Windows drops the native USB port during the successful reset. esptool
    # reports that expected re-enumeration as PermissionError, so do not pass
    # its return code back to PlatformIO.
    print("[post-upload] C3 watchdog reset sent; firmware is starting")


env.AddPostAction("upload", reset_with_watchdog)
