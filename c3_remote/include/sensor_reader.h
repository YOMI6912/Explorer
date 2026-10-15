#pragma once

#include <stdint.h>

#include "height_link_protocol.h"

namespace SensorReader {
bool begin();
HeightLink::Response measureHead(uint32_t requestId, uint32_t bootId);
}
