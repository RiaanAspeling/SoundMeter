#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <SPIFFS.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <AsyncElegantOTA.h>
#include <WebSerialLite.h>
#include <config.h>
#include "extensions.cpp"

AsyncWebServer server(80);
AsyncEventSource events("/events");
bool shouldSaveConfig = false;

int minSound = 4096;
int maxSound = 0;
int amplitude = 0;
int dbReading=0;

unsigned long lastReading = 0;
const long timerReading = 10L * 1000L;
unsigned long lastSubmit = 0;
const long timerSubmit = 30L * 1000L;

void DebugLog(String text) {
  Serial.println(text); 
  try {
    WebSerial.println(text);
  } catch (int e) {
    Serial.println("Unable to output to WebSerial. Error#" + String(e));
  }
}

void saveConfigFile()
{
  DebugLog(F("Saving config"));
  StaticJsonDocument<512> json;
  json["TS_HOST"] = TS_HOST;
  json["TS_PORT"] = TS_PORT;
  json["TS_CHANNEL"] = TS_CHANNEL;
  json["TS_APIKEY"] = TS_APIKEY;
  json["TS_SAMPLETIME"] = TS_SAMPLETIME;

  File configFile = SPIFFS.open(JSON_CONFIG_FILE, "w");
  if (!configFile)
  {
    DebugLog("failed to open config file for writing");
  }

  serializeJsonPretty(json, Serial);
  if (serializeJson(json, configFile) == 0)
  {
    DebugLog(F("Failed to write to file"));
  }
  configFile.close();
}

bool loadConfigFile()
{
  if (SPIFFS.exists(JSON_CONFIG_FILE))
  {
    //file exists, reading and loading
    DebugLog("reading config file");
    File configFile = SPIFFS.open(JSON_CONFIG_FILE, "r");
    if (configFile)
    {
      DebugLog("opened config file");
      StaticJsonDocument<512> json;
      DeserializationError error = deserializeJson(json, configFile);
      serializeJsonPretty(json, Serial);
      if (!error)
      {
        DebugLog("\nparsed json");

        strcpy(TS_HOST, json["TS_HOST"]);
        TS_PORT = json["TS_PORT"].as<int>();
        TS_CHANNEL = json["TS_CHANNEL"].as<long>();
        strcpy(TS_APIKEY, json["TS_APIKEY"]);
        TS_SAMPLETIME = json["TS_SAMPLETIME"].as<int>();
        return true;
      }
      else
      {
        DebugLog("failed to load json config");
      }
    }
  }
  //end read
  return false;
}

void GetBestReading(int sampleWindow, int *min, int *max, int *amp, int *db)
{
  digitalWrite(PIN_FOR_LED, HIGH);
  *min = 4096;
  *max = 0;
  unsigned long startMillis= millis();
  unsigned long samples = 0;
  unsigned long totalAmp = 0;
  long avgAmp = 0;
  while (millis() - startMillis < sampleWindow)
  {
    int sample = analogRead(PIN_FOR_SOUND);              // get reading from microphone
    if (sample > *max)
      *max = sample;                           // save just the max levels
    if (sample < *min)
      *min = sample;                           // save just the min levels
//    if (sample > 1000) {
      samples++;
      totalAmp += sample;
//    }
  }
  //*amp = (*max - *min);
  DebugLog("TotalAmp=" + String(totalAmp) + ", Samples#" + String(samples));
  *amp = totalAmp / samples;
  *db = -9 + (20 * log10(*amp));
  digitalWrite(PIN_FOR_LED, LOW);
}

void saveConfigCallback() {
  DebugLog("Should save config");
  shouldSaveConfig = true;
}

void configModeCallback(WiFiManager *myWiFiManager) {
  DebugLog("Entered Conf Mode");

  DebugLog("Config SSID: ");
  DebugLog(myWiFiManager->getConfigPortalSSID());

  DebugLog("Config IP Address: ");
  DebugLog(WiFi.softAPIP().toString());
}

void connectWifi(bool forceConfig) {
    // Setup wifi and manager
  WiFi.mode(WIFI_STA);
  WiFiManager wm;
  wm.setBreakAfterConfig(true);
  wm.setSaveConfigCallback(saveConfigCallback);
  wm.setAPCallback(configModeCallback);

  // Create custom data for configuration
  WiFiManagerParameter ts_addresstxt("TS_HOST", "Address/Host", TS_HOST, 128);    wm.addParameter(&ts_addresstxt);
  LongParameter ts_channeltxt("TS_CHANNEL", "Channel", TS_CHANNEL);               wm.addParameter(&ts_channeltxt);
  WiFiManagerParameter ts_apikeytxt("TS_APIKEY", "API Key", TS_APIKEY, 20);       wm.addParameter(&ts_apikeytxt);
  IntParameter ts_sampletimetxt("TS_SAMPLETIME", "Sample for ms", TS_SAMPLETIME); wm.addParameter(&ts_sampletimetxt);

  if (forceConfig) {
    if (!wm.startConfigPortal())
    {
      DebugLog("failed to connect and hit timeout");
      delay(5000);
      ESP.restart();
    }
  }
  else {
    bool canConnect = wm.autoConnect();
    
    if (!canConnect)
    {
      DebugLog("Failed to connect and hit timeout");
      delay(5000);
      ESP.restart();
    }
  }
  // Allow to reconnect to WiFi if signal is lost
  WiFi.setAutoReconnect(true);
  // Would be set with the callback saveConfigCallback
  if (shouldSaveConfig) {
    strncpy(TS_HOST, ts_addresstxt.getValue(), sizeof(TS_HOST));
    TS_CHANNEL = ts_channeltxt.getValue();
    strncpy(TS_APIKEY, ts_apikeytxt.getValue(), sizeof(TS_APIKEY));
    TS_SAMPLETIME = ts_sampletimetxt.getValue();
    saveConfigFile();
  }
}

void connectSPIFFS() {
  if (SPIFFS.begin(false) || SPIFFS.begin(true))
  {
    DebugLog("SPIFFS Connected!");
  } else {
    DebugLog("SPIFFS FAILED to mount!");
    delay(5000);
    ESP.restart();
  }
}

void webSerialMsg(uint8_t *data, size_t len){
  WebSerial.println("Received Data...");
  String d = "";
  for(int i=0; i < len; i++){
    d += char(data[i]);
  }
  WebSerial.println(d);
}

String convertJSON(int *db) {
  StaticJsonDocument<512> json;
  json["db"] = *db;
  String result;
  serializeJson(json, result);
  return result;
}

void uploadDataToTS(int min, int max, int amp, int db){
  WiFiClientSecure client;
  client.setInsecure();
  // Use WiFiClientSecure class to create SSL connection
  DebugLog("Connecting to   : " + String(TS_HOST));
  if (!client.connect(TS_HOST, TS_PORT)) {
    DebugLog("Connection failed");
    return;
  }
  String url = "/update?api_key=" + String(TS_APIKEY) + 
                                  "&field1=" + String(min) +
                                  "&field2=" + String(max) +
                                  "&field3=" + String(amp) +
                                  "&field4=" + String(db);
  DebugLog("Requesting      : "+url);
  client.print(String("GET ") + url + " HTTP/1.1\r\n" +
               "Host: " + TS_HOST + "\r\n" +
              "User-Agent: Sound-Sensor\r\n" +
               "Connection: close\r\n\r\n");
  DebugLog("Request sent    : ");
  while (client.connected()) {
    String line = client.readStringUntil('\n');
    if (line == "\r") {
      DebugLog("Headers received");
      break;
    }
  }
  String line = client.readStringUntil('\n');
  boolean Status = false;
  if (line.toInt() != 0) {
    line = "Server confirmed all data received";
    Status = true;
  }
  else
  {
    line = "Server responded with " + line;
  }
  
  DebugLog("Server Response : " + line);
  DebugLog("Status          : Closing connection");
}

void setupWebserver() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(SPIFFS, "/index.html", "text/html");
  });
  server.serveStatic("/", SPIFFS, "/");
  server.on("/readings", HTTP_GET, [](AsyncWebServerRequest *request){
    String json = convertJSON(&dbReading);
    request->send(200, "application/json", json);
    json = String();
  });
  events.onConnect([](AsyncEventSourceClient *client){
    client->send("hello!", NULL, millis(), 10000);
  });
  server.addHandler(&events);
  AsyncElegantOTA.begin(&server); // Add OTA update to web server
  WebSerial.begin(&server);
  WebSerial.onMessage(webSerialMsg);
  WebSerial.println("Starting ...");
  server.begin();
}

void setup() {
  pinMode(PIN_FOR_LED, OUTPUT);
  pinMode(PIN_FOR_SOUND, INPUT);   // Setup analog sensor pins
  pinMode(32, INPUT);
  digitalWrite(PIN_FOR_LED, HIGH);

  Serial.begin(115200);
  DebugLog("Starting...");

  connectSPIFFS();
  bool loadedConfig = loadConfigFile();
  connectWifi(!loadedConfig);

  setupWebserver();

  digitalWrite(PIN_FOR_LED, LOW);
}

void loop() {
  if ((millis() - lastReading) > timerReading) {
    // Send Events to the client with the Sensor Readings Every 10 seconds
    GetBestReading(TS_SAMPLETIME, &minSound, &maxSound, &amplitude, &dbReading);
    // Upload reading to any listening web browsers
    String reading = convertJSON(&dbReading);
    DebugLog(reading);
    events.send("ping",NULL,millis());
    events.send(reading.c_str(),"new_readings" ,millis());
    lastReading = millis();
    // Check if data should be submitted to online weather
    if ((millis() - lastSubmit) > timerSubmit) {
      lastSubmit = millis();
      uploadDataToTS(minSound, maxSound, amplitude, dbReading);
    }
  }
}