#pragma once

#include <stdint.h>

#include "height_link_protocol.h"

namespace WirelessLink {
struct Request {
  uint8_t mac[6];
  uint32_t id;
};

bool begin();
bool takeRequest(Request &out);
bool sendReply(const uint8_t *mac, const HeightLink::Response &reply);
void poll();
}
