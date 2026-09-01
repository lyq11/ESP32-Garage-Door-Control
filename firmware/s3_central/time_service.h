#pragma once

#include <Arduino.h>

void timeServiceBegin();
void timeServiceTick();
bool timeServiceReady();
String timeServiceLocalTime();
String timeServiceNextRestart();
bool timeServiceConsumeScheduledRestartMarker();
