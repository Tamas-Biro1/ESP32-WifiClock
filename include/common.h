#include <Arduino.h>

String readFile(String);
time_t getISRTimeVar();
void setISRTimeVar(time_t);
void startWifiAPMode();
void debugRTC();
void renderFace(float);
void writeFileToSDCard(String, String);
void handleRoot();
void incrementTime();
void handleClockFaceSetting();
void handleSetTimeSetting();
void handleWifiCredsSetting();
