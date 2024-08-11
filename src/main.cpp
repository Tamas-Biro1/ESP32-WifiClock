#include <assets.h>
#include <common.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <HTTPUpdateServer.h>
#include <EEPROM.h>
#include <soc/soc.h>
#include <soc/rtc_cntl_reg.h>
#include <Time.h>
#include <DS3232RTC.h>
#include <BH1750.h>
#include <SD.h>
#include "NTPTime.h"

// SPI3 on ESP32 S2 Mini for the SD card
#define SD_MOSI_PIN 34   // SPI master out, slave in
#define SD_MISO_PIN 36   // SPI master in, slave out
#define SD_SCK_PIN 38    // SPI clock
#define SD_CS_PIN 40     // SPI chip select for SD card reader

// I2C for RTC Clock and Light sensor
#define I2C_SDA_PIN 17    // I2C data pin
#define I2C_SCL_PIN 21    // I2C clock
#define RTC_1HZ_PIN 1     // DS3231 RTC provides a 1Hz interrupt signal on SQW to this pin

#define WIFI_LED_BLINK_FREQ 3000  // Blink frequency in sec
#define WIFI_LED_PIN 15           // internal blue power indicator LED
#define LCD_BL_PIN 2              // LCD backlight (PWM)

// Width and height of the screen
#define WIDTH  240
#define HEIGHT 240

// Calculate 1 second increment angles, results in smooth sub-pixel movement
#define SECOND_ANGLE 360.0 / 60.0
#define MINUTE_ANGLE SECOND_ANGLE / 60.0
#define HOUR_ANGLE   MINUTE_ANGLE / 12.0

#define AMBIENT_LIGHT_TRESHOLD_MAXIMUM 2.0
#define AMBIENT_LIGHT_MEASUREMENT_COUNT 50

#define WIFI_WAIT_TIMEOUT_MILLISEC 10000

// global variables
CLOCK_FACE clockFace;
DS3232RTC rtc;
BH1750 lightMeter;
File configFile;
tmElements_t actualRTCTime;
WebServer server(80);
HTTPUpdateServer httpUpdater;
String wifiSSID = "";
String wifiPass = "";
SPIClass& SD_SPI = SPI;
const char* hostName = "wificlock";     // wificlock.local will be the config page
unsigned char weekDays[7][12] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
float internalTimeSecs = 0 * 3600 + 0 * 60 + 0;
bool wifiLedState = false;
bool hasSD = true;
unsigned long previousWifiLedMillis = 0;
volatile time_t isrCurrentTime;

TFT_eSPI tft = TFT_eSPI();
TFT_eSprite bgSprite = TFT_eSprite(&tft);
TFT_eSprite hourHandSprite = TFT_eSprite(&tft);
TFT_eSprite minuteHandSprite = TFT_eSprite(&tft);
TFT_eSprite secondHandSprite = TFT_eSprite(&tft);

// light measurement array
float lightMeasurements[AMBIENT_LIGHT_MEASUREMENT_COUNT] = {0};
int measurementIndex = 0;
bool ambientLightArrayFilled = false;

// function prototypes
void debugTask(void *pvParameters);
void clockTask(void *pvParameters);
void wifiTask(void *pvParameters);
void lightTask(void *pvParameters);
void ntpTask(void *pvParameters);
void renderFace(float t);
void updateLightMeasurements(float newMeasurement);
float getAverageLightLevel();
void startWifiAPMode();
void handleRoot();
void handleClockFaceSetting();
void handleSetTimeSetting();
void handleWifiCredsSetting();
String readFile(String fileName);
void writeFileToSDCard(String filename, String content);
time_t getISRTimeVar();
void setISRTimeVar(time_t t);
bool initSDCard();
void initDisplay();
void incrementTime();

// The setup function where tasks are created
void setup() {
  // Disable brownout detector during startup
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  // Start serial debug
  Serial.begin(115200);
  Serial.println("Boot started...");
  Serial.println("Brownout disabled.");

  // initialize SD card, load settings
  hasSD = initSDCard();

  // initialize I2C bus with custom SDA and SCL pins
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

  // configure RTC
  pinMode(RTC_1HZ_PIN, INPUT_PULLUP);     // enable pullup on interrupt pin (SQW pin is open drain)
  attachInterrupt(digitalPinToInterrupt(RTC_1HZ_PIN), incrementTime, FALLING);    // set interrupt handler func
  rtc.begin();                              // start RTC communication on I2C
  rtc.squareWave(DS3232RTC::SQWAVE_1_HZ);   // set 1 Hz square wave output on SQW pin

  // ensure the ISR time variable is set with valid time on startup
  time_t t = getISRTimeVar();
  while (t == getISRTimeVar());             // wait for the next second (ISR trigger)
  t = rtc.get();                            // get the time from the RTC
  setISRTimeVar(t);                         // store ISR time to variable
  Serial.println("Time set from RTC");

  // look for light sensor on I2C bus
  lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE);
  Serial.println(F("BH1750 initialized."));

  // create FreeRTOS tasks
  xTaskCreatePinnedToCore(debugTask, "Debug Task", 8192, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(clockTask, "Clock Task", 8192, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(wifiTask, "WiFi Task", 8192, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(lightTask, "Light Task", 8192, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(ntpTask, "NTP Task", 8192, NULL, 1, NULL, 0);

  // enable brownout detector
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 1);
  Serial.println("Brownout enabled.");
}

// The loop function is no longer needed with FreeRTOS
void loop() {}

/* define FreeRTOS tasks */
// task to print debug variables
void debugTask(void *pvParameters) {
  for (;;) {
    debugRTC();
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

// task to update the clock face and hands
void clockTask(void *pvParameters) {  
  // init display and draw sprites
  initDisplay();

  // start running clock updater
  for (;;) {
    time_t t = getISRTimeVar();
    actualRTCTime.Hour = hour(t);
    actualRTCTime.Minute = minute(t);
    actualRTCTime.Second = second(t);
    internalTimeSecs = actualRTCTime.Hour * 3600 + actualRTCTime.Minute * 60 + actualRTCTime.Second;

    renderFace(internalTimeSecs);
    vTaskDelay(100 / portTICK_PERIOD_MS);
  }
}

// task to handle WiFi connection and server management
void wifiTask(void *pvParameters) {
  // Wifi status led (disabled)
  pinMode(WIFI_LED_PIN, OUTPUT);
  digitalWrite(WIFI_LED_PIN, LOW);

  // try to connect to AP with SSID:PASSWORD, if fails fall back to STA mode
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSSID.c_str(), wifiPass.c_str());
  unsigned long startTime = millis();
  while (WiFi.status() != WL_CONNECTED) {
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    Serial.println("Trying to connect to WiFi...");
    if (millis() - startTime > WIFI_WAIT_TIMEOUT_MILLISEC) {
      Serial.println("Failed to connect to WiFi, skipping on start...");
      startWifiAPMode();
      break;
    }
  }

  // configure callback functions
  server.on("/", handleRoot);
  server.on("/clockface", handleClockFaceSetting);
  server.on("/settime", handleSetTimeSetting);
  server.on("/setwificreds", handleWifiCredsSetting);

  // start mDNS responder
  MDNS.begin("wificlock");
  if (MDNS.begin("wificlock")) {
    Serial.println("mDNS responder started");
  }
  
  // start http server
  httpUpdater.setup(&server);
  server.begin();
  Serial.println("HTTP server started");
  MDNS.addService("http", "tcp", 80);

  // start listener task
  for (;;) {
    server.handleClient();
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

// task to handle light measurement and backlight control
void lightTask(void *pvParameters) {
  // init display backlight
  pinMode(LCD_BL_PIN, OUTPUT);
  digitalWrite(LCD_BL_PIN, HIGH);
  Serial.println("Backlight pin configured.");

  lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE);
  Serial.println(F("BH1750 initialized."));

  for (;;) {
    float lightLevel = lightMeter.readLightLevel();
    Serial.print("Light: ");
    Serial.print(lightLevel);
    Serial.println(" lx");
    updateLightMeasurements(lightLevel);
    float avgLightLevel = getAverageLightLevel();
    Serial.print("Average Light: ");
    Serial.print(avgLightLevel);
    Serial.println(" lx");

    if (avgLightLevel >= AMBIENT_LIGHT_TRESHOLD_MAXIMUM) {
      digitalWrite(LCD_BL_PIN, HIGH);
    } else {
      digitalWrite(LCD_BL_PIN, LOW);
    }
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

// task to synchronise local RTC with NTP
void ntpTask(void *pvParameters) {
  udp.begin(localPort);

  const TickType_t xDelay = pdMS_TO_TICKS(3600000);
  for (;;) {
    // if Wifi is connected, update the RTC chip clock
    if (WiFi.status() == WL_CONNECTED) {
      // decodeNTP() updates "actualNTPTime" local variable with NTP time
      WiFi.hostByName(ntpServerName, timeServerIP);
      sendNTPpacket(timeServerIP); // send an NTP packet to a time server
      decodeNTP();
      
      if ( no_packet_count > 0 )  {
        nextSendTime = millis() + 60 * 1000;
        Serial.println("Failed to synchronize with NTP, retrying in 1 minute.");
      } else {
        rtc.set(makeTime(actualNTPTime));
        Serial.println("RTC was updated using NTP time.");
      }
    } else {
      Serial.println("WiFi not connected, skipping NTP sync.");
    }

    // delay task for 1 hour
    vTaskDelay(xDelay);
  }
}

/* define functions */
// initialize SD card, load settings
bool initSDCard() {
  SD_SPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN);
  Serial.println("SPI bus initialized");
  if (! SD.begin(SD_CS_PIN, SD_SPI)) {
    Serial.println("SD card initialization failed!");
    return false;
  }
  Serial.println("SD card initialization done, loading settings...");

  // read clock face
  String clockFaceSetting = readFile("/clockfaceconf");
  if(clockFaceSetting != "") {
    clockFace = CLOCK_FACES[clockFaceSetting.toInt()];
  }

  // read Wifi credentials for connecting to AP
  String wifiCredentials = readFile("/wificonf");
  if (wifiCredentials != "") {
    int colonPos = wifiCredentials.indexOf(':');
    if (colonPos != -1) {
      wifiSSID = wifiCredentials.substring(0, colonPos);
      wifiPass = wifiCredentials.substring(colonPos + 1);
      Serial.println(wifiSSID + " " + wifiPass);
    } else {
      Serial.println("Error: Wifi credential must be in 'SSID:PASSWORD' format!");
    }
  }
  return true;
}

// initialize TFT
void initDisplay() {
  tft.init();
  tft.setRotation(3);
  tft.fillScreen(TFT_WHITE);
  tft.setSwapBytes(true);
  Serial.println("LCD configured.");

  // background
  bgSprite.setColorDepth(16);
  bgSprite.createSprite(WIDTH, HEIGHT);
  bgSprite.fillSprite(TFT_WHITE);
  bgSprite.setSwapBytes(true);
  bgSprite.pushImage(0, 0, WIDTH, HEIGHT, clockFace.BACKGROUND);
  bgSprite.pushSprite(0,0);
  
  // hour
  hourHandSprite.setColorDepth(8);
  hourHandSprite.createSprite(clockFace.HOUR_HAND_WIDTH, clockFace.HOUR_HAND_HEIGHT);
  hourHandSprite.fillSprite(TFT_WHITE);
  hourHandSprite.setSwapBytes(true);
  hourHandSprite.setPivot(clockFace.HOUR_HAND_CENTER_X, clockFace.HOUR_HAND_CENTER_Y);
  hourHandSprite.pushImage(0, 0, clockFace.HOUR_HAND_WIDTH, clockFace.HOUR_HAND_HEIGHT, clockFace.HOUR_HAND);
  hourHandSprite.pushSprite((WIDTH / 2) - clockFace.HOUR_HAND_CENTER_X, (HEIGHT/2) - clockFace.HOUR_HAND_CENTER_Y, TFT_BLUE);

  // minute
  minuteHandSprite.setColorDepth(8);
  minuteHandSprite.createSprite(clockFace.MINUTE_HAND_WIDTH, clockFace.MINUTE_HAND_HEIGHT);
  minuteHandSprite.fillSprite(TFT_WHITE);
  minuteHandSprite.setSwapBytes(true);
  minuteHandSprite.setPivot(clockFace.MINUTE_HAND_CENTER_X, clockFace.MINUTE_HAND_CENTER_Y);
  minuteHandSprite.pushImage(0, 0, clockFace.MINUTE_HAND_WIDTH, clockFace.MINUTE_HAND_HEIGHT, clockFace.MINUTE_HAND);
  minuteHandSprite.pushSprite((WIDTH / 2) - clockFace.MINUTE_HAND_CENTER_X, (HEIGHT/2) - clockFace.MINUTE_HAND_CENTER_Y, TFT_BLUE);

  // second
  secondHandSprite.setColorDepth(8);
  secondHandSprite.createSprite(clockFace.SECOND_HAND_WIDTH, clockFace.SECOND_HAND_HEIGHT);
  secondHandSprite.fillSprite(TFT_WHITE);
  secondHandSprite.setSwapBytes(true);
  secondHandSprite.setPivot(clockFace.SECOND_HAND_CENTER_X, clockFace.SECOND_HAND_CENTER_Y);
  secondHandSprite.pushImage(0, 0, clockFace.SECOND_HAND_WIDTH, clockFace.SECOND_HAND_HEIGHT, clockFace.SECOND_HAND);
  secondHandSprite.pushSprite((WIDTH / 2) - clockFace.SECOND_HAND_CENTER_X, (HEIGHT/2) - clockFace.SECOND_HAND_CENTER_Y, TFT_BLUE);
}

// start Wifi in AP mode
void startWifiAPMode() {
  Serial.println("Starting Wifi in AP mode...");
  WiFi.mode(WIFI_AP);                                       // start Wifi in Access Point mode
  WiFi.softAP(hostName, "da3EcI5G198lCdzak35", 1, 1, 2);    // set AP SSID and password
}

// blink built-in blue led to indicate Wifi is established in STA mode
void blinkWifiLed() {
  unsigned long currentWifiLedMillis = millis();
  if (currentWifiLedMillis - previousWifiLedMillis >= (WIFI_LED_BLINK_FREQ)) {
    previousWifiLedMillis = currentWifiLedMillis;
    wifiLedState = !wifiLedState;
    digitalWrite(WIFI_LED_PIN, wifiLedState);
  }
}

// update measurements array with new values
void updateLightMeasurements(float newMeasurement) {
  lightMeasurements[measurementIndex] = newMeasurement;
  measurementIndex = (measurementIndex + 1) % AMBIENT_LIGHT_MEASUREMENT_COUNT;
  if (measurementIndex == 0) {
    ambientLightArrayFilled = true;
  }
}

// get avg light level from measurements array
float getAverageLightLevel() {
  float sum = 0;
  int count = ambientLightArrayFilled ? AMBIENT_LIGHT_MEASUREMENT_COUNT : measurementIndex;
  for (int i = 0; i < count; i++) {
    sum += lightMeasurements[i];
  }
  return sum / count;
}

// render clock hands positions on display
void renderFace(float t) {
  float h_angle = t * HOUR_ANGLE;
  float m_angle = t * MINUTE_ANGLE;
  float s_angle = t * SECOND_ANGLE; 
  
  bgSprite.pushImage(0, 0, WIDTH, HEIGHT, clockFace.BACKGROUND);
  hourHandSprite.pushRotated(&bgSprite,h_angle, TFT_BLUE);
  hourHandSprite.pushImage(0, 0, clockFace.HOUR_HAND_WIDTH, clockFace.HOUR_HAND_HEIGHT, clockFace.HOUR_HAND);
  minuteHandSprite.pushRotated(&bgSprite,m_angle, TFT_BLUE);
  minuteHandSprite.pushImage(0, 0, clockFace.MINUTE_HAND_WIDTH, clockFace.MINUTE_HAND_HEIGHT, clockFace.MINUTE_HAND);
  secondHandSprite.pushRotated(&bgSprite,s_angle, TFT_BLUE);
  secondHandSprite.pushImage(0, 0, clockFace.SECOND_HAND_WIDTH, clockFace.SECOND_HAND_HEIGHT, clockFace.SECOND_HAND);
  bgSprite.pushSprite(0, 0, TFT_BLUE);
}

// handler function for root page
void handleRoot() {
  String html = "<!DOCTYPE html><body>";
  html += "<form action='/clockface' method='POST'>";
  html += "<label for='radio1'><b>Select clock face:</b></label><br>";
  html += "<input type='radio' name='radio' value='0' id='radio1'>BigBen<br>";
  html += "<input type='radio' name='radio' value='1' id='radio2'>SBB Clock<br>";
  html += "<br><input type='submit' value='Save'>";
  html += "</form>";
  html += "<hr><br>";
  html += "<form action='/settime' method='POST'>";
  html += "<label for='radio1'><b>Set time:</b></label><br>";
  html += "<input type='number' size='6' name='year' min='2000' max='2070' value='2023'>Year<br>";
  html += "<input type='number' size='6' name='month' min='1' max='12' value='1'>Month<br>";
  html += "<input type='number' size='6' name='day' min='1' max='31' value='1'>Day<br>";
  html += "<input type='number' size='6' name='hour' min='0' max='23' value='0'>Hour<br>";
  html += "<input type='number' size='6' name='minute' min='0' max='59' value='0'>Minute<br>";
  html += "<input type='number' size='6' name='second' min='0' max='59' value='0'>Second<br>";
  html += "<br><input type='submit' value='Save'>";
  html += "</form>";
  html += "<hr><br>";
  html += "<form action='/setwificreds' method='POST'>";
  html += "<label for='radio1'><b>Set Wifi credentials for STA mode:</b></label><br>";
  html += "<input type='text' size='15' name='wifissid'>SSID<br>";
  html += "<input type='password' size='15' name='wifipasswd'>Password<br>";
  html += "<br><input type='submit' value='Save'>";
  html += "</form>";
  html += "<hr><br>";
  html += "<button onclick=\"window.location.href='/update';\">Firmware update...</button>";
  html += "<hr><br>";
  server.send(200, "text/html", html);
}

// handler function for clock face change
void handleClockFaceSetting() {
  if(hasSD)
  {
    if (server.arg("radio") == "0") {
      writeFileToSDCard("/clockfaceconf", "0");
    } else if(server.arg("radio") == "1") {
      writeFileToSDCard("/clockfaceconf", "1");
    }  

    server.send(200, "text/html", "Clock face variable set. Restarting...");
    delay(1000);
    ESP.restart();
  } else {
    server.send(200, "text/html", "No SD card!");
  }
}

// handler function for set time
void handleSetTimeSetting() {
  tmElements_t t;
  t.Year = CalendarYrToTm(static_cast<uint8_t>(server.arg("year").toInt()));
  t.Month = static_cast<uint8_t>(server.arg("month").toInt());
  t.Day = static_cast<uint8_t>(server.arg("day").toInt());
  t.Hour = static_cast<uint8_t>(server.arg("hour").toInt());
  t.Minute = static_cast<uint8_t>(server.arg("minute").toInt());
  t.Second = static_cast<uint8_t>(server.arg("second").toInt());
  rtc.set(makeTime(t));     // update RTC chip time on UI
  setISRTimeVar(makeTime(t));
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", ""); // empty body for 302 response
  Serial.println("RTC was updated by user input");
}

// handler function for wifi credentials
void handleWifiCredsSetting() {
  if(hasSD)
  {
    String wifiCreds = server.arg("wifissid")+":"+ server.arg("wifipasswd");
    writeFileToSDCard("/wificonf", wifiCreds);
    server.send(200, "text/html", "Wifi credentials changed. Restarting...");
    delay(1000);
    ESP.restart();
  } else {
    server.send(200, "text/html", "No SD card!");
  }
}

// read the first line of the given file
String readFile(String fileName) {
  String firstLine = "";
  if (!SD.begin(SD_CS_PIN, SD_SPI)) {
    Serial.println("SD card initialization failed.");
    return firstLine;
  }
  configFile = SD.open(fileName, FILE_READ);
  if (configFile) {
    firstLine = configFile.readStringUntil('\n');
    configFile.close();
  } else {
    Serial.println("Failed to open file for reading.");
  }
  return firstLine;
}

// write file with content to the SD
void writeFileToSDCard(String filename, String content) {
  if (!SD.begin(SD_CS_PIN, SD_SPI)) {
    Serial.println("SD card initialization failed.");
    return;
  }

  // check if the file exists
  if (!SD.exists(filename)) {
    // create the file if it doesn't exist
    File file = SD.open(filename, FILE_WRITE);
    if (!file) {
      Serial.println("Error creating file.");
      return;
    }
    file.close();
  }

  // open the file for writing
  File file = SD.open(filename, FILE_WRITE);
  
  if (!file) {
    Serial.println("Error opening file for writing.");
    return;
  }
  
  // write the content to the file
  file.print(content);
  
  // close the file
  file.close();
  
  Serial.println("File written successfully.");
}

void debugRTC()
{
  /*Serial.print(actualRTCTime.Year(), DEC);
  Serial.print('/');
  Serial.print(actualRTCTime.Month(), DEC);
  Serial.print('/');
  Serial.print(actualRTCTime.Day(), DEC);
  Serial.print(" (");*/
  Serial.print(actualRTCTime.Hour, DEC);
  Serial.print(':');
  Serial.print(actualRTCTime.Minute, DEC);
  Serial.print(':');
  Serial.print(actualRTCTime.Second, DEC);
  Serial.println();
}

// return current time (ISR)
time_t getISRTimeVar()
{
    noInterrupts();
    time_t t = isrCurrentTime;
    interrupts();
    return t;
}

// set the current time (ISR)
void setISRTimeVar(time_t t)
{
    noInterrupts();
    isrCurrentTime = t;
    interrupts();
}

// RTC interrupt handler
void incrementTime()
{
    ++isrCurrentTime;
}