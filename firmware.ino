#include <Arduino.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <HTTPClient.h>

#include <DNSServer.h>
#include <WebServer.h>
#include <WiFiManager.h>

#include <Adafruit_NeoPixel.h>

#include <SPI.h>
#include <SD.h>

#include <ArduinoHttpClient.h>
#include <ArduinoJson.h>

#include <Time.h>
#include <TimeLib.h>
#include <WiFiUdp.h>

// VL6180 Sensor Drivers
#include <Wire.h>
#include "Adafruit_VL6180X.h"

Adafruit_VL6180X vl = Adafruit_VL6180X();

// MCP9808 Sensor Drivers
#include <Adafruit_Sensor.h>
#include "Adafruit_MCP9808.h"

Adafruit_MCP9808 tempsensor = Adafruit_MCP9808();

float tempOffset = 1;

/* Wifi Setup */
#include "constants.h"
int wifiStatus = WL_IDLE_STATUS;
WiFiClient wifi;

HttpClient client = HttpClient(wifi, API_ENDPOINT, API_PORT);

/* Sensor & LED & Button Setup */
int feedingButtonPin = 14;
int recordingSwitchPin = 32;

int feedingButtonState = 0;
int previousFeedingButtonState = 0;

int uploadButtonState = 0;
int previousUploadButtonState = 0;

int sensorValue = 0;

/* NeoPixel Setup */
int neoPixelPin = 15;

int defaultRed = 0, defaultGreen = 0, defaultBlue = 0, defaultBrightness = 0;

Adafruit_NeoPixel pixels(1, neoPixelPin, NEO_RGB + NEO_KHZ800);

/* Sensor Readings */
float temp, pressure, humidity, altitude, lux;
uint32_t gas;
uint8_t height;

/* NTP Setup */
WiFiUDP Udp;
unsigned int localPort = 8888;

static const char ntpServerName[] = "time.google.com";
time_t getNtpTime();
void sendNTPpacket(IPAddress &address);
time_t timeStamp = 0;
const int timeZone = 0; // UTC
const int NTP_PACKET_SIZE = 48; // NTP time is in the first 48 bytes of message
byte packetBuffer[NTP_PACKET_SIZE]; //buffer to hold incoming & outgoing packets

/* Timing Config */
unsigned long previousSampleMillis = 0;
const long sampleInterval = (60.0/SAMPLES_PER_MINUTE) * 1000;

unsigned long previousUploadMillis = 0;
unsigned long uploadInterval;

/* Uploading & Logging */
char jsonToSend[5000];
int uploadAttempts = 0;
bool lastUploadAttempt = true;

const String outgoingDir = "/outgoing";
const String uploadedDir = "/uploaded";

String jsonPayload;

/* SD Card */
#define cardSelect 33

void setup() {
  Serial.begin(115200);

  printBootMessages();

  pixels.begin();
  pixels.show();

  // Turn on Lux LED to illuminate sensor:
  // pixels.setPixelColor(0, pixels.Color(255, 75, 0));

  for (int x = 0; x <= 128; x++) {
    pixels.setPixelColor(0, pixels.Color(255, 75, 0));
    pixels.setBrightness(x);
    pixels.show();
    delay(5);
  }

  for (int x = 32; x <= 64; x += 4) {
    pixels.clear();
    pixels.show();
    delay(pow(1.1, x));

    pixels.setPixelColor(0, pixels.Color(255, 75, 0));
    pixels.show();
    delay(pow(1.1, x));
  }

  while (!Serial) { ; }

  connectToNet();

  initSensors();

  syncNTP();

  setupPins();

  resetUploadTimer();

  initializeSDCard();

  pixels.clear();

}

void loop() {
  sampleAndRecordTimer();
  uploadTimer();
  feedingButtonCheck();
  defaultLED();
}

void connectToNet() {

  WiFiManager wifiManager;
  wifiManager.autoConnect("Breadwinner");

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  Serial.print("Connected! IP address: ");
  Serial.println(WiFi.localIP());
}

/* setup() Helper Functions */
void printBootMessages() {
  Serial.println(F("----------------"));
  Serial.println(F("Breadwinner initializing..."));
}

void setupPins() {
  pinMode(feedingButtonPin, INPUT_PULLDOWN);
  pinMode(recordingSwitchPin, INPUT);
}

void initSensors() {
  initVL6180();
  initMCP9808();
}

bool initVL6180() {
 if (!vl.begin()) {
    Serial.println(F("Failed to find VL6180 sensor."));
    return false;
  }
  Serial.println(F("VL6180 sensor initialized."));
  return true;
}

bool initMCP9808() {
  if (!tempsensor.begin(0x18)) {
    Serial.println(F("Could not find a valid MCP9808 sensor."));
    return false;
  }

  tempsensor.setResolution(3);

  Serial.println(F("MCP9808 sensor initialized."));
  return true;
}

void initializeSDCard() {
  if (!SD.begin(cardSelect)) {
    Serial.println(F("Card failed, or not present"));
    while (1);
  }
  Serial.println(F("SD card and reader initialized."));

  Serial.print("Trying to create directory: ");
  Serial.println(outgoingDir);
  if (SD.mkdir(outgoingDir)) {
    Serial.print(outgoingDir);
    Serial.println(" successfully created.");
  } else {
    Serial.println("Couldn't create ");
    Serial.print(outgoingDir);
  }

  Serial.print("Trying to create directory: ");
  Serial.println(uploadedDir);
  if (SD.mkdir(uploadedDir)) {
    Serial.print(uploadedDir);
    Serial.println(" successfully created.");
  } else {
    Serial.println("Couldn't create ");
    Serial.print(uploadedDir);
  }
}

void getTemperatureOffset() {
  // File tempOffsetFile = SD.open("/temperature_offset", FILE_READ);

  // if (tempOffsetFile) {
  //   while(tempOffsetFile.available()) {
  //     char c = tempOffsetFile.read();
  //     // Serial.println(c);
  //     tempOffset = tempOffset + c;
  //   }
  //   Serial.print("Temp offset: " );
  //   Serial.println(tempOffset);
  // } else {
  //   Serial.println(F("Temp offset file is empty or unavailable."));
  // }
  // tempOffsetFile.close();
}

void feedingButtonCheck() {
  feedingButtonState = digitalRead(feedingButtonPin);
  if (feedingButtonState == LOW) {
    previousFeedingButtonState = HIGH;
  } else {
    if (previousFeedingButtonState != LOW) {
      Serial.println("Feeding button!");
      recordFeeding();
      uploadInterval = 0; // Force upload after feeding
      previousFeedingButtonState = LOW;
    }
  }
}

void uploadTimer() {
  unsigned long currentUploadMillis = millis();
  if (currentUploadMillis - previousUploadMillis >= uploadInterval) {
    previousUploadMillis = currentUploadMillis;
    Serial.print(F("Upload attempt #"));
    Serial.println(uploadAttempts);
    lastUploadAttempt = uploadData(VITALS) && uploadData(FEEDINGS);
    if (lastUploadAttempt) {
      resetUploadTimer();
      uploadAttempts = 0;
    } else {
      // Serial.println(F("Resetting wifi..."));
      Serial.println(F("Trying to reconnect..."));
      WiFi.begin();
      delay(1000);
      if (uploadAttempts >= 10) {
        Serial.println(F("Max upload attempts reached."));
      } else {
        Serial.print(F("Old uploadInterval: "));
        Serial.println(uploadInterval);
        uploadInterval = (pow(2, uploadAttempts) * 2000) + ((60.0/UPLOADS_PER_HOUR)*1000) + random(0, 1000);
        Serial.print(F("New uploadInterval: "));
        Serial.println(uploadInterval);
      }
      uploadAttempts++;
    }
  }
}

void blinkLED(String color) {

  int red, green, blue;

  if (color == "red") {
    red = 255;
    green = 0;
    blue = 0;
  } else if (color == "green") {
    red = 0;
    green = 255;
    blue = 0;
  } else if (color == "blue") {
    red = 0;
    green = 0;
    blue = 255;
  } else if (color == "orange") {
    red = 255;
    green = 75;
    blue = 0;
  } else {
    red = 0;
    green = 0;
    blue = 0;
  }

  pixels.setBrightness(128);
  pixels.setPixelColor(0, pixels.Color(red, green, blue));
  pixels.show();

  if (color != "off") {
    delay(200);
  }
}

void defaultLED() {

  pixels.setBrightness(defaultBrightness);
  pixels.setPixelColor(0, pixels.Color(defaultRed, defaultGreen, defaultBlue));
  pixels.show();
}

void sampleAndRecordTimer() {
  // Serial.println("sampleAndRecordTimer();");
  unsigned long currentSampleMillis = millis();
  if (currentSampleMillis - previousSampleMillis >= sampleInterval) {
    previousSampleMillis = currentSampleMillis;
    blinkLED("orange");
    sampleSensors();
    recordSensorData();
    blinkLED("green");
  }
}

void sampleSensors() {
  if (!sampleMCP9808()) {
    Serial.println(F("Error sampling MCP9808!"));
    initMCP9808();
  };

  if (!sampleVL6180()) {
    Serial.println(F("Error sampling VL6180!"));
    initVL6180();
  };
}

bool sampleVL6180() {
  lux = vl.readLux(VL6180X_ALS_GAIN_5);
  height = vl.readRange();

  uint8_t status = vl.readRangeStatus();

  if (status == VL6180X_ERROR_NONE) {
    return true;
  }

  if  ((status >= VL6180X_ERROR_SYSERR_1) && (status <= VL6180X_ERROR_SYSERR_5)) {
    Serial.println(F("VL6180 System error"));
  }
  else if (status == VL6180X_ERROR_ECEFAIL) {
    Serial.println(F("VL6180 ECE failure"));
  }
  else if (status == VL6180X_ERROR_NOCONVERGE) {
    Serial.println(F("VL6180 No convergence"));
  }
  else if (status == VL6180X_ERROR_RANGEIGNORE) {
    Serial.println(F("VL6180 Ignoring range"));
  }
  else if (status == VL6180X_ERROR_SNR) {
    Serial.println(F("VL6180 Signal/Noise error"));
  }
  else if (status == VL6180X_ERROR_RAWUFLOW) {
    Serial.println(F("VL6180 Raw reading underflow"));
  }
  else if (status == VL6180X_ERROR_RAWOFLOW) {
    Serial.println(F("VL6180 Raw reading overflow"));
  }
  else if (status == VL6180X_ERROR_RANGEUFLOW) {
    Serial.println(F("VL6180 Range reading underflow"));
  }
  else if (status == VL6180X_ERROR_RANGEOFLOW) {
    Serial.println(F("VL6180 Range reading overflow"));
  }
  return false;
}

bool sampleMCP9808() {
  tempsensor.wake();

  // These delay()'s seem necessary to get different readings
  // See: https://forums.adafruit.com/viewtopic.php?f=19&t=86088
  delay(253);

  temp = tempsensor.readTempF() - tempOffset;

  // shutdown MSP9808 - power consumption ~0.1 mikro Ampere, stops temperature
  tempsensor.shutdown_wake(1);

  delay(1000);

  return true;
}

void resetUploadTimer() {
  Serial.println(F("Resetting upload timer."));
  uploadInterval = (60.0/UPLOADS_PER_HOUR) * 60 * 1000;
}

bool uploadData(String dataName) {
  Serial.print(F("Attempting to sync "));
  Serial.println(dataName);

  if (WiFi.status() == WL_CONNECTED) {
    if (endpointOnline()) {
      Serial.println(F("Endpoint reachable."));
    } else {
      Serial.println(F("Endpoint not reachable. "));
      return false;
    }
  } else {
    Serial.println(F("Wifi not online."));
    return false;
  }

  Serial.println(F("Opening output file..."));
  const String localFilePath = outgoingDir + "/" + dataName;

  File localFile = SD.open(localFilePath, FILE_READ);

  const size_t capacity = JSON_ARRAY_SIZE(UPLOAD_BATCH_SIZE + 1) + JSON_OBJECT_SIZE(1) + (UPLOAD_BATCH_SIZE + 1)*JSON_OBJECT_SIZE(10);
  DynamicJsonDocument doc(capacity);
  JsonArray records = doc.createNestedArray(dataName);

  String row;
  int totalRows = 0;

  if (localFile) {
    while(localFile.available()) {
      char c = localFile.read();
      if (c == '\n') {
        totalRows++;
      }
    }

    int rowCursor = 0;

    localFile.seek(0);

    while(localFile.available()) {
      char c = localFile.read();
      if (c == '\n') {
        records.add(serialized(row));
        if (
          (totalRows < UPLOAD_BATCH_SIZE && rowCursor == totalRows) // File is smaller than batch size
          || ((rowCursor) % UPLOAD_BATCH_SIZE == 0) // Processing a batch
          || ((totalRows - rowCursor) < UPLOAD_BATCH_SIZE && rowCursor == totalRows) // Last of the file
        ) {

          Serial.print(F("Uploading "));
          Serial.print(rowCursor);
          Serial.print(F(" out of "));
          Serial.print(totalRows);
          Serial.println(F(" total rows."));

          serializeJson(doc, jsonToSend);

          if(sendMetric(API_PATH + String(dataName + "/batch_upload"))) {
            Serial.print(F("Uploading batch "));
            Serial.println(F("Batch upload success!"));
          } else {
            Serial.println(F("Batch upload errored!"));
          }
          doc.clear();
          records = doc.createNestedArray(dataName);
        }
        row = "";
        rowCursor++;
      } else {
        row = row + c;
      }
    }

    doc.clear();

    String archiveFilename = uploadedDir + "/" + year() + printDigits(month()) + printDigits(day());

    File archiveFile = SD.open(archiveFilename, FILE_APPEND);

    if (archiveFile) {
      localFile.seek(0);

      Serial.print(F("Opened "));
      Serial.print(archiveFilename);
      Serial.println(F(" for writing ..."));

      Serial.print(archiveFilename);
      Serial.print(F(" file size before writing: "));
      Serial.println(String(archiveFile.size()));

      long fileSize = 0;
      size_t n;
      uint8_t buf[64];
      while ((n = localFile.read(buf, sizeof(buf))) > 0) {
        archiveFile.write(buf, n);
        fileSize = fileSize + sizeof(buf);
      }

      Serial.print(F("Attempted to write "));
      Serial.print(String(fileSize));
      Serial.print(F(" bytes to: "));
      Serial.println(archiveFilename);

      archiveFile.close();
      localFile.close();

      if (SD.remove(localFilePath)) {
        Serial.print(F("Removed "));
        Serial.println(localFilePath);
      } else {
        Serial.print(F("Error deleting "));
        Serial.println(localFilePath);
      }
    } else {
      Serial.print(F("Error opening "));
      Serial.print(archiveFilename);
      Serial.println(F(" for writing."));
    }
    archiveFile.close();
  } else {
    Serial.print(dataName);
    Serial.println(F(" file is empty or unavailable."));
  }
  localFile.close();

  doc.clear();

  blinkLED("off");

  return true;
}

void recordSensorData() {
  if (timeStatus() != timeNotSet) {
    if (now() != timeStamp) {
      timeStamp = now();
    }
  }

  DynamicJsonDocument jsonDoc(JSON_OBJECT_SIZE(10));

  jsonDoc["device_id"] = DEVICE_ID;
  jsonDoc["observed_at"] = (long)timeStamp;
  jsonDoc["wifi_rssi"] = WiFi.RSSI();

  jsonDoc["temperature"] = temp;
  jsonDoc["height"] = height;
  jsonDoc["lux"] = lux;

  char observation[200];
  serializeJson(jsonDoc, observation);

  jsonDoc.clear();

  // SdFile::dateTimeCallback(dateTime);
  const String localFilePath = outgoingDir + "/" + VITALS;
  File dataFile = SD.open(localFilePath, FILE_APPEND);

  if (dataFile) {
    dataFile.println(observation);
    dataFile.close();
    Serial.print(F("Wrote "));
    Serial.print(observation);
    Serial.print(F(" to "));
    Serial.println(localFilePath);
    blinkLED("green");
  } else {
    Serial.print(F("Error opening: "));
    Serial.println(localFilePath);
    blinkLED("red");
  }
}

void recordFeeding() {
  Serial.println(F("Feeding button pressed!"));
  blinkLED("blue");

  if (timeStatus() != timeNotSet) {
    if (now() != timeStamp) {
      timeStamp = now();
    }
  }

  StaticJsonDocument<200> jsonDoc;

  jsonDoc["device_id"] = DEVICE_ID;
  jsonDoc["fed_at"] = (long)timeStamp;

  String feeding;
  serializeJson(jsonDoc, feeding);

  // SdFile::dateTimeCallback(dateTime);
  const String localFilePath = outgoingDir + "/" + FEEDINGS;
  File dataFile = SD.open(localFilePath, FILE_APPEND);

  if (dataFile) {
    dataFile.println(feeding);
    dataFile.close();
    Serial.print(F("Wrote: "));
    Serial.print(feeding);
    Serial.print(F(" to "));
    Serial.println(localFilePath);
    blinkLED("green");
  } else {
    Serial.print(F("Error opening file: "));
    Serial.println(localFilePath);
    blinkLED("red");
  }

  jsonDoc.clear();
}


void syncNTP() {
  Serial.println(F("Starting UDP"));
  Udp.begin(localPort);

  while (timeStatus() == timeNotSet) {
    Serial.println(F("Waiting for NTP sync..."));
    setSyncProvider(getNtpTime);
    setSyncInterval(300);
    if ( timeStatus() == timeSet) {
      Serial.print("Time set: ");
      printTimeStamp();
    }
    delay(500);
  }
}

bool endpointOnline() {

  client.setTimeout(1);
  client.get(HEALTH_ENDPOINT);

  int statusCode = client.responseStatusCode();

  String response = client.responseBody();

  Serial.print(F("Endpoint status: "));
  Serial.println(statusCode);
  Serial.print(F("Endpoint response: "));
  Serial.println(response);

  return (statusCode == 200);
}

bool sendMetric(String path) {

  blinkLED("blue");

  Serial.print(F("POST'ing to "));
  Serial.print(API_ENDPOINT);
  Serial.print(path);
  Serial.print(F(":"));
  Serial.println(jsonToSend);

  String contentType = "application/json";
  client.post(path, contentType, jsonToSend);

  int statusCode = client.responseStatusCode();
  String response = client.responseBody();

  Serial.print(F("Status code: "));
  Serial.println(statusCode);

  Serial.print(F("Response: "));
  Serial.println(response);

  StaticJsonDocument<200> doc;
  DeserializationError error = deserializeJson(doc, response);

  if (doc.containsKey("red")) {
    defaultRed = doc["red"];
    defaultBlue = doc["blue"];
    defaultGreen = doc["green"];
    defaultBrightness = doc["brightness"];
    defaultLED();
  }

  if (statusCode == 200) {
    Serial.println("Upload OK!");
    blinkLED("green");
    return true;
  } else {
    Serial.println("Upload Error!");
    blinkLED("red");
    return false;
  }
}

void printTimeStamp() {
  Serial.println(iso8601());
}

String iso8601(){
  return "" + String(year()) + "-" +
    printDigits(month()) + "-" +
    printDigits(day()) + "T" +
    printDigits(hour()) + ":" +
    printDigits(minute()) + ":" +
    printDigits(second()) + "Z";
}

String printDigits(int digits){
  if(digits < 10){
    return '0' + String(digits);
  } else {
    return String(digits);
  }
}

/* NTP code */
time_t getNtpTime() {
  IPAddress ntpServerIP; // NTP server's ip address

  while (Udp.parsePacket() > 0) ; // discard any previously received packets
  Serial.println(F("Transmit NTP Request"));
  // get a random server from the pool
  WiFi.hostByName(ntpServerName, ntpServerIP);
  Serial.print(ntpServerName);
  Serial.print(": ");
  Serial.println(ntpServerIP);
  sendNTPpacket(ntpServerIP);
  uint32_t beginWait = millis();
  while (millis() - beginWait < 2500) {
    int size = Udp.parsePacket();
    if (size >= NTP_PACKET_SIZE) {
      Serial.print("Received NTP Response: ");
      Udp.read(packetBuffer, NTP_PACKET_SIZE);  // read packet into the buffer
      unsigned long secsSince1900;
      // convert four bytes starting at location 40 to a long integer
      secsSince1900 =  (unsigned long)packetBuffer[40] << 24;
      secsSince1900 |= (unsigned long)packetBuffer[41] << 16;
      secsSince1900 |= (unsigned long)packetBuffer[42] << 8;
      secsSince1900 |= (unsigned long)packetBuffer[43];
      Serial.println(secsSince1900);
      // Convert to UTC / Unix Epoch:
      return secsSince1900 - 2208988800UL + timeZone * SECS_PER_HOUR;
    }
  }
  Serial.println(F("No NTP Response :-("));
  return 0; // return 0 if unable to get the time
}

void sendNTPpacket(IPAddress &address) {
  // set all bytes in the buffer to 0
  memset(packetBuffer, 0, NTP_PACKET_SIZE);
  // Initialize values needed to form NTP request
  // (see URL above for details on the packets)
  packetBuffer[0] = 0b11100011;   // LI, Version, Mode
  packetBuffer[1] = 0;     // Stratum, or type of clock
  packetBuffer[2] = 6;     // Polling Interval
  packetBuffer[3] = 0xEC;  // Peer Clock Precision
  // 8 bytes of zero for Root Delay & Root Dispersion
  packetBuffer[12] = 49;
  packetBuffer[13] = 0x4E;
  packetBuffer[14] = 49;
  packetBuffer[15] = 52;
  // all NTP fields have been given values, now
  // you can send a packet requesting a timestamp:
  Udp.beginPacket(address, 123); //NTP requests are to port 123
  Udp.write(packetBuffer, NTP_PACKET_SIZE);
  Udp.endPacket();
}

