#include "devices/UsbPrinter.h"

#include "config.h"
#include "esp_err.h"
#include "esp_intr_alloc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb/usb_host.h"

#include <math.h>
#include <string.h>

namespace UsbPrinter {
namespace {
usb_host_client_handle_t clientHandle = nullptr;
usb_device_handle_t printerHandle = nullptr;

volatile uint8_t newDeviceAddress = 0;
volatile bool deviceGone = false;
bool printRunning = false;
bool interfaceClaimed = false;
bool printerReady = false;
bool hostStarted = false;

portMUX_TYPE transferStateMux = portMUX_INITIALIZER_UNLOCKED;
TransferResult transferResult = TransferResult::None;

constexpr size_t TICKET_BUFFER_SIZE = 512;

struct TicketBuffer {
  uint8_t data[TICKET_BUFFER_SIZE] = {};
  size_t length = 0;

  bool appendByte(uint8_t value) {
    if (length >= sizeof(data)) {
      return false;
    }
    data[length++] = value;
    return true;
  }

  bool appendText(const char *value) {
    if (value == nullptr) {
      return false;
    }
    const size_t valueLength = strlen(value);
    if (valueLength > sizeof(data) - length) {
      return false;
    }
    memcpy(data + length, value, valueLength);
    length += valueLength;
    return true;
  }

  bool appendSafeAscii(const String &value, size_t maximumLength) {
    const size_t count = value.length() < maximumLength
                             ? value.length()
                             : maximumLength;
    if (count > sizeof(data) - length) {
      return false;
    }

    for (size_t index = 0; index < count; ++index) {
      const uint8_t character = static_cast<uint8_t>(value.charAt(index));
      data[length++] = character >= 0x20 && character <= 0x7E
                           ? character
                           : static_cast<uint8_t>('?');
    }
    return true;
  }
};

void finishTransfer(TransferResult result) {
  portENTER_CRITICAL(&transferStateMux);
  transferResult = result;
  printRunning = false;
  portEXIT_CRITICAL(&transferStateMux);
}

bool beginTransfer() {
  bool accepted = false;
  portENTER_CRITICAL(&transferStateMux);
  if (!printRunning) {
    printRunning = true;
    transferResult = TransferResult::None;
    accepted = true;
  }
  portEXIT_CRITICAL(&transferStateMux);
  return accepted;
}

void closePrinter() {
  printerReady = false;

  portENTER_CRITICAL(&transferStateMux);
  if (printRunning) {
    transferResult = TransferResult::Failed;
    printRunning = false;
  }
  portEXIT_CRITICAL(&transferStateMux);

  if (printerHandle == nullptr) {
    interfaceClaimed = false;
    return;
  }

  if (interfaceClaimed) {
    usb_host_interface_release(clientHandle,
                               printerHandle,
                               Config::Printer::INTERFACE_NUMBER);
  }

  usb_host_device_close(clientHandle, printerHandle);
  printerHandle = nullptr;
  interfaceClaimed = false;
  Serial.println("[PRINTER] Device closed");
}

void transferCallback(usb_transfer_t *transfer) {
  const bool completed =
      transfer->status == USB_TRANSFER_STATUS_COMPLETED &&
      transfer->actual_num_bytes == transfer->num_bytes;

  if (completed) {
    Serial.printf("[PRINTER] Print success: %d bytes\n",
                  transfer->actual_num_bytes);
  } else {
    Serial.printf("[PRINTER] Print error, status=%d bytes=%d/%d\n",
                  transfer->status,
                  transfer->actual_num_bytes,
                  transfer->num_bytes);
  }

  finishTransfer(completed ? TransferResult::Printed
                           : TransferResult::Failed);
  usb_host_transfer_free(transfer);
}

void clientEvent(const usb_host_client_event_msg_t *eventMessage, void *) {
  switch (eventMessage->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
      newDeviceAddress = eventMessage->new_dev.address;
      Serial.printf("[PRINTER] USB device found, address=%u\n",
                    newDeviceAddress);
      break;

    case USB_HOST_CLIENT_EVENT_DEV_GONE:
      deviceGone = true;
      Serial.println("[PRINTER] USB device disconnected");
      break;

    default:
      break;
  }
}

bool openPrinter(uint8_t address) {
  esp_err_t error =
      usb_host_device_open(clientHandle, address, &printerHandle);
  if (error != ESP_OK) {
    Serial.printf("[PRINTER] Open error: %s\n", esp_err_to_name(error));
    printerHandle = nullptr;
    return false;
  }

  const usb_device_desc_t *descriptor = nullptr;
  error = usb_host_get_device_descriptor(printerHandle, &descriptor);
  if (error != ESP_OK || descriptor == nullptr) {
    Serial.println("[PRINTER] Cannot read USB descriptor");
    closePrinter();
    return false;
  }

  Serial.printf("[PRINTER] VID=0x%04X PID=0x%04X\n",
                descriptor->idVendor,
                descriptor->idProduct);

  if (descriptor->idVendor != Config::Printer::USB_VENDOR_ID ||
      descriptor->idProduct != Config::Printer::USB_PRODUCT_ID) {
    Serial.println("[PRINTER] USB device does not match config.h");
    closePrinter();
    return false;
  }

  error = usb_host_interface_claim(clientHandle,
                                   printerHandle,
                                   Config::Printer::INTERFACE_NUMBER,
                                   Config::Printer::ALTERNATE_SETTING);
  if (error != ESP_OK) {
    Serial.printf("[PRINTER] Interface claim error: %s\n",
                  esp_err_to_name(error));
    closePrinter();
    return false;
  }

  interfaceClaimed = true;
  printerReady = true;
  Serial.println("[PRINTER] WINMAX-8032 ready");

  delay(Config::Printer::READY_DELAY_MS);
  if (Config::Printer::PRINT_TEST_WHEN_CONNECTED) {
    printTest();
  }
  return true;
}

void hostTask(void *) {
  while (true) {
    uint32_t eventFlags = 0;
    usb_host_lib_handle_events(portMAX_DELAY, &eventFlags);

    if (eventFlags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
      usb_host_device_free_all();
    }
  }
}

void clientTask(void *) {
  while (true) {
    usb_host_client_handle_events(clientHandle, portMAX_DELAY);

    if (newDeviceAddress != 0 && printerHandle == nullptr) {
      const uint8_t address = newDeviceAddress;
      newDeviceAddress = 0;
      openPrinter(address);
    }

    if (deviceGone) {
      deviceGone = false;
      closePrinter();
    }
  }
}
}  // namespace

bool begin() {
  if (hostStarted) {
    return true;
  }

  Serial.println("[PRINTER] Starting USB Host...");

  usb_host_config_t hostConfig = {};
  hostConfig.skip_phy_setup = false;
  hostConfig.intr_flags = ESP_INTR_FLAG_LEVEL1;

  esp_err_t error = usb_host_install(&hostConfig);
  if (error != ESP_OK) {
    Serial.printf("[PRINTER] USB Host error: %s\n", esp_err_to_name(error));
    return false;
  }

  usb_host_client_config_t clientConfig = {};
  clientConfig.is_synchronous = false;
  clientConfig.max_num_event_msg = Config::Printer::MAX_EVENT_MESSAGES;
  clientConfig.async.client_event_callback = clientEvent;
  clientConfig.async.callback_arg = nullptr;

  error = usb_host_client_register(&clientConfig, &clientHandle);
  if (error != ESP_OK) {
    Serial.printf("[PRINTER] USB Client error: %s\n", esp_err_to_name(error));
    return false;
  }

  if (xTaskCreate(hostTask,
                  "USB Host",
                  Config::Printer::HOST_TASK_STACK,
                  nullptr,
                  Config::Printer::HOST_TASK_PRIORITY,
                  nullptr) != pdPASS) {
    Serial.println("[PRINTER] Cannot create USB Host task");
    return false;
  }

  if (xTaskCreate(clientTask,
                  "USB Client",
                  Config::Printer::CLIENT_TASK_STACK,
                  nullptr,
                  Config::Printer::CLIENT_TASK_PRIORITY,
                  nullptr) != pdPASS) {
    Serial.println("[PRINTER] Cannot create USB Client task");
    return false;
  }

  hostStarted = true;
  Serial.println("[PRINTER] Waiting for configured USB printer...");
  return true;
}

void update() {
  // USB events run in FreeRTOS tasks. Keep this function so main.cpp has
  // the same begin()/update() pattern as the other device modules.
}

bool isReady() {
  return printerReady;
}

bool isBusy() {
  portENTER_CRITICAL(&transferStateMux);
  const bool busy = printRunning;
  portEXIT_CRITICAL(&transferStateMux);
  return busy;
}

TransferResult takeTransferResult() {
  portENTER_CRITICAL(&transferStateMux);
  const TransferResult result = transferResult;
  transferResult = TransferResult::None;
  portEXIT_CRITICAL(&transferStateMux);
  return result;
}

bool write(const uint8_t *data, size_t length) {
  if (!printerReady || printerHandle == nullptr || data == nullptr || length == 0) {
    Serial.println("[PRINTER] Printer not ready or print data is empty");
    return false;
  }

  if (!beginTransfer()) {
    Serial.println("[PRINTER] Print transfer busy");
    return false;
  }

  usb_transfer_t *transfer = nullptr;
  esp_err_t error = usb_host_transfer_alloc(length, 0, &transfer);
  if (error != ESP_OK || transfer == nullptr) {
    finishTransfer(TransferResult::Failed);
    Serial.printf("[PRINTER] Transfer allocation error: %s\n",
                  esp_err_to_name(error));
    return false;
  }

  memcpy(transfer->data_buffer, data, length);
  transfer->num_bytes = length;
  transfer->device_handle = printerHandle;
  transfer->bEndpointAddress = Config::Printer::ENDPOINT_OUT;
  transfer->callback = transferCallback;
  transfer->context = nullptr;

  error = usb_host_transfer_submit(transfer);
  if (error != ESP_OK) {
    finishTransfer(TransferResult::Failed);
    usb_host_transfer_free(transfer);
    Serial.printf("[PRINTER] Transfer submit error: %s\n",
                  esp_err_to_name(error));
    return false;
  }

  Serial.printf("[PRINTER] Submitted %u bytes\n",
                static_cast<unsigned int>(length));
  return true;
}

bool printQueueTicket(const String &queueNumber,
                      float weightKg,
                      float heightCm,
                      float bmi,
                      const String &measuredAt) {
  if (queueNumber.isEmpty() ||
      !isfinite(weightKg) || weightKg <= 0.0f ||
      !isfinite(heightCm) || heightCm <= 0.0f ||
      !isfinite(bmi) || bmi <= 0.0f) {
    Serial.println("[PRINTER] Invalid queue ticket data");
    return false;
  }

  char measurementLine[48] = {};
  TicketBuffer ticket;

  bool built = true;
  built &= ticket.appendByte(0x1B);  // ESC @ : initialize
  built &= ticket.appendByte(0x40);
  built &= ticket.appendByte(0x1B);  // ESC a 1 : center
  built &= ticket.appendByte(0x61);
  built &= ticket.appendByte(0x01);
  built &= ticket.appendByte(0x1B);  // ESC E 1 : bold on
  built &= ticket.appendByte(0x45);
  built &= ticket.appendByte(0x01);
  built &= ticket.appendText("CLINIC QUEUE\n");
  built &= ticket.appendByte(0x1D);  // GS ! 0x11 : double size
  built &= ticket.appendByte(0x21);
  built &= ticket.appendByte(0x11);
  built &= ticket.appendSafeAscii(queueNumber, 24);
  built &= ticket.appendText("\n");
  built &= ticket.appendByte(0x1D);  // GS ! 0 : normal size
  built &= ticket.appendByte(0x21);
  built &= ticket.appendByte(0x00);
  built &= ticket.appendByte(0x1B);  // ESC E 0 : bold off
  built &= ticket.appendByte(0x45);
  built &= ticket.appendByte(0x00);
  built &= ticket.appendText("--------------------------------\n");
  built &= ticket.appendByte(0x1B);  // ESC a 0 : left
  built &= ticket.appendByte(0x61);
  built &= ticket.appendByte(0x00);
  built &= ticket.appendText("Queue : ");
  built &= ticket.appendSafeAscii(queueNumber, 24);
  built &= ticket.appendText("\n");

  snprintf(measurementLine,
           sizeof(measurementLine),
           "Weight: %.1f kg\n",
           static_cast<double>(weightKg));
  built &= ticket.appendText(measurementLine);
  snprintf(measurementLine,
           sizeof(measurementLine),
           "Height: %.1f cm\n",
           static_cast<double>(heightCm));
  built &= ticket.appendText(measurementLine);
  snprintf(measurementLine,
           sizeof(measurementLine),
           "BMI   : %.2f\n",
           static_cast<double>(bmi));
  built &= ticket.appendText(measurementLine);
  built &= ticket.appendText("Time  : ");
  if (measuredAt.isEmpty()) {
    built &= ticket.appendText("-");
  } else {
    built &= ticket.appendSafeAscii(measuredAt, 40);
  }
  built &= ticket.appendText("\n--------------------------------\n");
  built &= ticket.appendText("Please wait for your queue\n");
  built &= ticket.appendText("Thank you\n");
  built &= ticket.appendByte(0x1B);  // ESC d n : feed n lines
  built &= ticket.appendByte(0x64);
  built &= ticket.appendByte(Config::Printer::PAPER_FEED_LINES);

  if (!built) {
    Serial.println("[PRINTER] Queue ticket exceeds print buffer");
    return false;
  }

  return write(ticket.data, ticket.length);
}

bool printTest() {
  const uint8_t printData[] = {
      0x1B, 0x40,              // ESC @ : initialize
      0x1B, 0x61, 0x01,        // center
      'W', 'I', 'N', 'M', 'A', 'X', '-', '8', '0', '3', '2', '\n',
      'E', 'S', 'P', '3', '2', '-', 'S', '3', '\n',
      'U', 'S', 'B', ' ', 'H', 'O', 'S', 'T', '\n',
      '\n',
      0x1B, 0x61, 0x00,        // left
      'T', 'E', 'S', 'T', ' ', 'P', 'R', 'I', 'N', 'T', '\n',
      'W', 'e', 'i', 'g', 'h', 't', ':', ' ',
      '5', '2', '.', '4', '0', ' ', 'k', 'g', '\n',
      'H', 'e', 'i', 'g', 'h', 't', ':', ' ',
      '1', '6', '5', '.', '0', ' ', 'c', 'm', '\n',
      'P', 'R', 'I', 'N', 'T', 'E', 'R', ' ', 'R', 'E', 'A', 'D', 'Y', '\n',
      0x1B, 0x64, Config::Printer::PAPER_FEED_LINES};

  return write(printData, sizeof(printData));
}

}  // namespace UsbPrinter
