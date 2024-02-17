#include "Arduino.h"
#pragma once

#define JSON_CONFIG_FILE    "/config.json"
#define PIN_FOR_LED         5
#define PIN_FOR_SOUND       33

char TS_HOST[128]   = "api.thingspeak.com";
int  TS_PORT        = 443;
long TS_CHANNEL     = 1234567;
char TS_APIKEY[20]  = "0123456789ABCDEF";
int  TS_SAMPLETIME  = 5000;
