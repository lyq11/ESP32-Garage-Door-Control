#pragma once

#include <Arduino.h>

void webApiBegin();
void webApiHandleClient();
String systemStatusJson();

extern bool setupApRunning;
extern bool webReady;
extern bool wifiReady;
extern String lastCommandResult;
