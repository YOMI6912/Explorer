# ESP32-S3 Clinic Modular

Production firmware for the clinic scale:

- `src/` and `include/`: ESP32-S3 firmware
- `c3_remote/`: ESP32-C3 height sensor firmware
- `platformio.ini`: S3 production build

Build S3:

```powershell
pio run -e esp32-s3-clinic
```

Build C3 from `c3_remote/`:

```powershell
Copy-Item include/clinic_network_local.example.h include/clinic_network_local.h
pio run -e esp32-c3-vl53-remote
```

Set the local hotspot credentials only in the ignored `clinic_network_local.h` file.
