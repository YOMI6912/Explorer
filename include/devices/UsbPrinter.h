#pragma once

#include <Arduino.h>

namespace UsbPrinter {

enum class TransferResult : uint8_t {
  None,
  Printed,
  Failed
};

bool begin();
void update();
bool isReady();
bool isBusy();

TransferResult takeTransferResult();

bool write(const uint8_t *data, size_t length);
bool printQueueTicket(const String &queueNumber,
                      float weightKg,
                      float heightCm,
                      float bmi,
                      const String &measuredAt);
bool printTest();

} 