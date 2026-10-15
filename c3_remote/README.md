# C3 height link production firmware

1. Copy `include/clinic_network_local.example.h` to `include/clinic_network_local.h`.
2. Set the local hotspot name and password in that file.
3. Build with `pio run -e esp32-c3-vl53-remote`.

The firmware serves the calibrated VL53L1X height link and returns measurements to the S3 over TCP.
