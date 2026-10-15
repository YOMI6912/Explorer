#pragma once

#include <Arduino.h>

namespace OnlineQueueWorkflow {

void begin();
void setMeasurementHardwareReady(bool loadCellReady, bool heightSensorReady);
void refreshDisplay();

void update();

void handleKey(char key);

bool isActive();
bool isNetworkReady();

bool isReadyForMeasurement();
bool submitMeasurement(float weightKg, float heightCm);

bool submitWalkInMeasurement(float weightKg, float heightCm);

}
