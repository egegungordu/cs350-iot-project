#include "arduinoFFT.h"
#include <EEPROM.h>
#include <ESP8266WiFi.h>
#include <ESP8266WiFiMulti.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecureBearSSL.h>

#define SAMPLES 128             //Must be a power of 2
#define SAMPLING_FREQUENCY 4000 //Hz, must be less than 10000 due to ADC
#define WAIT_TIME 2000
#define BUFFER_SIZE 64 // Max 255
#define BUTTON D1
#define LED D6
#define COOLDOWN 3000
#define DETECT_THRESHOLD 0.6

#define RECORD_LED_BLINK 0

const char* ssid = "gungordu";
const char* password = "31033103";
const char* host = "https://vdjavr1c59.execute-api.eu-central-1.amazonaws.com/default/notify";
const uint8_t fingerprint[20] = {0x4c, 0x01, 0xcf, 0x00, 0xbb, 0x89, 0xa5, 0xef, 0xe7, 0x09, 0xb0, 0xb5, 0xe6, 0xa5, 0xb1, 0x8b, 0xe3, 0x7c, 0x83, 0x1a};

arduinoFFT FFT = arduinoFFT();
ESP8266WiFiMulti WiFiMulti;

unsigned int sampling_period_us;
unsigned long microseconds;

double vReal[SAMPLES];
double vImag[SAMPLES];

int lastPeak;
int recorded[BUFFER_SIZE];
int last[BUFFER_SIZE];
double recordedSampleCount;

enum state {
  WAITING,
  RECORDING,
  LISTENING,
  SLEEPING
};

enum state currentState;
unsigned long timestamp;
unsigned long ledTimestamp;
bool ledOn;

void writeIntArrayIntoEEPROM(int address, int numbers[], int arraySize)
{
  int addressIndex = address;
  for (int i = 0; i < arraySize; i++) 
  {
    EEPROM.write(addressIndex, numbers[i] >> 8);
    EEPROM.write(addressIndex + 1, numbers[i] & 0xFF);
    addressIndex += 2;
  }
  EEPROM.commit();
}

void readIntArrayFromEEPROM(int address, int numbers[], int arraySize)
{
  int addressIndex = address;
  for (int i = 0; i < arraySize; i++)
  {
    numbers[i] = (EEPROM.read(addressIndex) << 8) + EEPROM.read(addressIndex + 1);
    addressIndex += 2;
  }
}

void setup(void){
  pinMode(D1, INPUT);
  pinMode(D6, OUTPUT);
  Serial.begin(500000);
  EEPROM.begin(512);
  
  for (uint8_t t = 4; t > 0; t--) {
    Serial.printf("[SETUP] WAIT %d...\n", t);
    Serial.flush();
    delay(1000);
  }
  WiFi.mode(WIFI_STA);
  WiFiMulti.addAP(ssid, password);
  Serial.println(WiFi.localIP());
  
  timestamp = millis();
  sampling_period_us = round(1000000*(1.0/SAMPLING_FREQUENCY));
  readIntArrayFromEEPROM(2, recorded, BUFFER_SIZE); 
  Serial.println("Recorded array: ");
  recordedSampleCount = 0;
  for(int i = 0; i < BUFFER_SIZE; i++) {
    Serial.println(recorded[i]);
    if(recorded[i] != 0){
      recordedSampleCount++;
    }
  }
}

void ledBlink(int rate) {
  if(millis() - ledTimestamp > rate) {
    ledOn = !ledOn;
    ledTimestamp = millis();
    digitalWrite(LED, ledOn);
  }
}

void matchedLedBlink() {
  bool wasOn = false;
  for(int i = 0; i < 20; i++) {
    digitalWrite(LED, wasOn);
    wasOn = !wasOn;
    delay(25);
  }
}

void doWait() {
  if (millis() - timestamp > WAIT_TIME) {
    currentState = LISTENING;
  }
}

void reconnect() {
  WiFiMulti.run();
}

void notify() {
  if ((WiFiMulti.run() == WL_CONNECTED)) {
    std::unique_ptr<BearSSL::WiFiClientSecure>client(new BearSSL::WiFiClientSecure);
    //client->setInsecure();
    client->setFingerprint(fingerprint);
    HTTPClient https;
    Serial.print("[HTTPS] begin...\n");
    if (https.begin(*client, host)) {

      Serial.print("[HTTPS] POST...\n");
      https.addHeader("Content-Type", "application/json");
      int httpCode = https.POST("{\"topic\":\"doorbell\",\"title\":\"Doorbell IoT\",\"text\":\"Someone is at the door!\",\"password\":\"supersecretpassword\"}");

      if (httpCode > 0) {
        Serial.printf("[HTTPS] POST... code: %d\n", httpCode);
        if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_MOVED_PERMANENTLY) {
          String payload = https.getString();
          Serial.println(payload);
        }
      } else {
        Serial.printf("[HTTPS] GET... failed, error: %s\n", https.errorToString(httpCode).c_str());
      }
      https.end();
    } else {
      Serial.printf("[HTTPS] Unable to connect\n");
    }
  } else {
    Serial.println("Wifi is disconnected, retrying...");
    delay(1000);
    notify();
  }
}

void doRecord() {
  if(digitalRead(BUTTON)) {
    currentState = LISTENING;
    timestamp = millis();
    writeIntArrayIntoEEPROM(2, recorded, 64);
    return;
  }
  recordedSampleCount = 0;
  int peak = calculatePeak();
  for(int i=0; i<BUFFER_SIZE-1; i++){
    recorded[i] = recorded[i+1];
    if(recorded[i] != 0) {
      recordedSampleCount++;
    }
  }
  if(peak != 0) {
    recordedSampleCount++;
  }
  recorded[BUFFER_SIZE-1] = peak;
  Serial.println(recorded[BUFFER_SIZE-1]);
  ledBlink(RECORD_LED_BLINK);
}

void doListen() {
  if(!digitalRead(BUTTON)) {
    currentState = RECORDING;
    for(int i=0; i<BUFFER_SIZE; i++){
      recorded[i] = 0;
    }
    return;
  }
  int peak = calculatePeak();
  double errorCount = 0;
  for(int i=0; i<BUFFER_SIZE-1; i++){
    if(recorded[i] != 0 && abs(last[i] - recorded[i]) > 6) {
      errorCount++;
    }
    last[i] = last[i+1];
  }
  last[BUFFER_SIZE-1] = peak;
  Serial.println(1 - errorCount/recordedSampleCount);
  if(1-errorCount/recordedSampleCount > DETECT_THRESHOLD) {
    currentState = SLEEPING;
    for(int i=0; i<BUFFER_SIZE; i++){
      last[i] = 0;
    }
    timestamp = millis();
    matchedLedBlink();
    Serial.println("DETECTED");
    notify();
    return;
  }
  digitalWrite(LED, 1);
  reconnect();
}

void doSleep() {
  if (millis() - timestamp > COOLDOWN) {
    currentState = LISTENING;
    return;
  }
}

int calculatePeak() {
  for(int i=0; i<SAMPLES; i++)
  {
      microseconds = micros();    //Overflows after around 70 minutes!
      vReal[i] = analogRead(0);
      vImag[i] = 0;
      while(micros() < (microseconds + sampling_period_us)){}
  }
  FFT.Windowing(vReal, SAMPLES, FFT_WIN_TYP_HAMMING, FFT_FORWARD);
  FFT.Compute(vReal, vImag, SAMPLES, FFT_FORWARD);
  FFT.ComplexToMagnitude(vReal, vImag, SAMPLES);
  double peak = FFT.MajorPeak(vReal, SAMPLES, SAMPLING_FREQUENCY);
  if(peak < 20) {
    return 0;
  }
  while(peak < 250) { peak *= 2; }
  while(peak > 510) { peak /= 2; }
  // NEED THIS SMILIARITY CHECK WHEN USED ON 5V
  bool similar = abs(lastPeak - peak) < 20;
  lastPeak = peak;
  if(similar) {
    return peak;
  } else {
    return 0;
  }
}

void loop(void){
  switch(currentState){
    case WAITING:
      doWait();
      break;
    case RECORDING:
      doRecord();
      break;
    case LISTENING:
      doListen();
      break;
    case SLEEPING:
      doSleep();
      break;
  }
}
