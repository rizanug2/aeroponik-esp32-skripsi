
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <BH1750.h>
#include <EEPROM.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "esp_adc_cal.h"
#include <DHT.h>
#include <RTClib.h>
#include <esp_task_wdt.h>

// === IOT INCLUDES ===
#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include <addons/TokenHelper.h>
#include <addons/RTDBHelper.h>

// ============ CONFIGURATION ============
#define WDT_TIMEOUT 30 

// Kredensial WiFi & Firebase
#define WIFI_SSID "NAMA_WIFI"
#define WIFI_PASSWORD "PASSWORD_WIFI"
#define API_KEY "API_KEY_FIREBASE"
#define DATABASE_URL "URL_FIREBASE"

// ============ PIN DEFINITIONS ============
#define I2C_LCD_SDA  18
#define I2C_LCD_SCL  19
#define I2C_SENS_SDA 21
#define I2C_SENS_SCL 22

#define BTN_LEFT_PIN   5
#define BTN_RIGHT_PIN  4
#define BTN_ENTER_PIN  16
#define BTN_BACK_PIN   17
#define BUZZER_PIN     2

// Relay
#define RELAY_NUTRI_AB_PIN  26
#define RELAY_MIXER_PIN     25
#define RELAY_SAMPLING_PIN  14
#define RELAY_BUANG_PIN     27
#define RELAY_PH_UP_PIN     13
#define RELAY_PH_DOWN_PIN   12
#define RELAY_SPRAY_PIN     33
#define RELAY_LAMPU_PIN     32

// Sensors
#define PH_PIN 34
#define EC_PIN 35
#define OW_PIN 23
#define DHT_PIN 15
#define DHT_TYPE DHT22

// ============ OBJECTS ============
LiquidCrystal_I2C lcd(0x27, 20, 4);
RTC_DS3231 rtc;
BH1750 lightSensor;
DHT dht(DHT_PIN, DHT_TYPE);
OneWire oneWire(OW_PIN);
DallasTemperature DS18B20(&oneWire);
esp_adc_cal_characteristics_t *adc_chars;

// Firebase Objects
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;
bool signupOK = false;

// ============ GLOBAL VARIABLES (UPDATED) ============
// 100ml = 65 detik -> 1.54 ml/detik

// Nutrisi AB 
const int WAKTU_TAMBAH_NUTRISI_MS = 3000; 
// Mixing (Tetap)
const int WAKTU_MIXING_MS         = 10000; 
// Isi Wadah Sensor (Target misal 30ml agar probe terendam) -> butuh 20 detik
const int WAKTU_ISI_WADAH_SENSOR_MS = 245000; //245 detik=4 menit an
// Stabilisasi Sensor (Tetap)
const int WAKTU_STABIL_SENSOR_MS  = 10000;  
// Buang Sampel (Harus lebih lama dari isi, misal target 25 detik agar tuntas)
const int WAKTU_BUANG_SAMPLING_MS = 265000;  //265 detik = 4 menit an

// Default Setpoint
const float DEFAULT_SP_PH = 6.1; 
const float DEFAULT_SP_EC = 2000.0;
const int DEFAULT_SP_INT_SPRAY = 15; // Interval (Menit)
const int DEFAULT_SP_DUR_SPRAY = 300; // Durasi (Detik)

// Settingan lainnya
const int DEFAULT_SP_INT_CHECK = 6; // Cek Nutrisi tiap 6 Jam (Bisa ubah 1 jam via menu kalo mau test)
const float DEFAULT_SP_LUX = 5000.0;
const int DEFAULT_SP_DOSING = 1500; //dosing ph
const float DEFAULT_SP_MAX_TEMP = 35.0; 
const unsigned long DISPLAY_INTERVAL = 1000;

unsigned long dynamicNutriDuration = 3000; // Default awal

// Sync Timer
unsigned long lastFirebaseSync = 0;
const int SYNC_INTERVAL = 2000; // Kirim data tiap 2 detik
unsigned long lastModeChangeTime = 0;

enum MenuLevel { MODE_MENU, MAIN_MENU, SETTING_MENU, CALIB_MENU, SETPOINT_MENU, MANUAL_TEST_MENU, RTC_MENU, FACTORY_RESET_MENU };
MenuLevel currentLevel = MODE_MENU;

const int MAIN_MENU_ITEMS = 4;
const int SETTING_MENU_ITEMS = 5; 
const int CALIB_MENU_ITEMS = 2;
const int SETPOINT_MENU_ITEMS = 8; 

int mainIndex = 0; int settingIndex = 0; int calIndex = 0; int spIndex = 0;
int manualIndex = 0;
const int MANUAL_ITEMS = 8; 

int rtcEditState = 0; 
int tempHour = 0; int tempMin = 0;

bool inCalibration = false;
bool inSetpointAdjust = false;
bool waitingForBackAfterSave = false;
bool waitingForAckAfterSampling = false;
bool alreadySprayedThisInterval = false;
bool alreadySampledThisHour = false; 
bool cursorInAutoMode = false; 
bool modeAuto = false; 
bool inFactoryResetConfirm = false;

unsigned long lastBackPressTime = 0;
int backClickCount = 0;
const int DOUBLE_CLICK_SPEED = 500; 
bool backBtnReleased = true; 
const unsigned long btnDebounceDelay = 50; 
bool lastStateLeft = HIGH; unsigned long lastBtnTimeLeft = 0;
bool lastStateRight = HIGH; unsigned long lastBtnTimeRight = 0;
bool lastStateEnter = HIGH; unsigned long lastBtnTimeEnter = 0;
bool lastStateBack = HIGH; unsigned long lastBtnTimeBack = 0;
bool isBuzzerOn = false;
unsigned long buzzerStartTime = 0;

enum ProcessState { 
  PROC_IDLE, PROC_SAMPLING_ADD_NUTRI, PROC_SAMPLING_MIXING, 
  PROC_SAMPLING_FILL_POT, PROC_SAMPLING_READ, PROC_SAMPLING_DRAIN, 
  PROC_SAMPLING_DRAIN_BEFORE_CORRECT, PROC_CORRECT_PH_UP, 
  PROC_CORRECT_PH_DOWN, PROC_WAIT_AFTER_CORRECTION, PROC_SPRAYING
};
ProcessState currentProcess = PROC_IDLE;
unsigned long processStartTime = 0;
int retryCount = 0; 
const int MAX_RETRY = 5;  //jumlah siklus sampling

unsigned long lastInputTime = 0;
unsigned long idleDelay = 5000; 
bool autoLoopActive = false;
unsigned long lastLoopChange = 0;
int loopPage = 0;
unsigned long lastDisplay = 0;

unsigned long lastDSRequest = 0;
const int DS_DELAY_MS = 800; 
bool firstRunDS = true;      
const int EC_SAMPLES = 20;
const int PH_SAMPLES = 100;

float sensor_pH = 0.0, sensor_EC_uScm = 0.0, sensor_temp = 0.0;
float sensor_lux = 0.0, sensor_temp_dht = 0.0, sensor_hum_dht = 0.0;
float EC = 0.0, pH = 0.0, tempAir = 0.0, lux = 0.0, tempUdara = 0.0, humUdara = 0.0; 

float offset_pH = 0.0, offset_EC = 0.0, Kvalue = 1.0, slope = 0.0, intercept = 0.0;
String calType = "";
String calA = "";
String calB = "";
int calibrationStep = 0;
float phCalA_val = 7.0; float phCalB_val = 4.0; 
float ecCalA_val = 12880.0; float ecCalB_val = 1413.0;  
float calVoltage_Step1 = 0.0; 
bool isCalibSampling = false;        
unsigned long calibStartTime = 0;    
const int CALIB_DURATION_MS = 5000;  
float calVoltageAccumulator = 0.0;    
int calSampleCount = 0;               
float tempFixedVoltage = 0.0;  
bool lastSamplingReady = false;
float lastSamplingEC = 0.0;
float lastSamplingPH = 0.0;

float setpoint_pH = DEFAULT_SP_PH; float setpointPH = DEFAULT_SP_PH;          
float setpoint_EC_uS = DEFAULT_SP_EC; float setpointEC = DEFAULT_SP_EC;        
int setpointSprayIntervalMenit = DEFAULT_SP_INT_SPRAY; 
int setpointSprayDurationDetik = DEFAULT_SP_DUR_SPRAY; 
int setpointSampleIntervalJam = DEFAULT_SP_INT_CHECK;  
float setpointLux = DEFAULT_SP_LUX;        
int setpointDosingDur_ms = DEFAULT_SP_DOSING;    
float setpointMaxTempWater = DEFAULT_SP_MAX_TEMP; 

int currentDigitIndex = 0;
char digitsBuf[6] = {'0','0','0','0','\0'};
String currentType = "";
char digitList[12] = {'0','1','2','3','4','5','6','7','8','9','.','\0'};
String inputLine = ""; 

// EEPROM Addresses
const int ADDR_pH_offset = 0;
const int ADDR_EC_offset = ADDR_pH_offset + sizeof(float);
const int ADDR_Kvalue = ADDR_EC_offset + sizeof(float);
const int ADDR_slope = ADDR_Kvalue + sizeof(float);
const int ADDR_intercept = ADDR_slope + sizeof(float);
const int ADDR_setpoint_pH = ADDR_intercept + sizeof(float);
const int ADDR_setpoint_EC = ADDR_setpoint_pH + sizeof(float);
const int ADDR_MODE = ADDR_setpoint_EC + sizeof(float);
const int ADDR_sp_interval = ADDR_MODE + sizeof(int);
const int ADDR_sp_duration = ADDR_sp_interval + sizeof(int);
const int ADDR_sp_sample_interval = ADDR_sp_duration + sizeof(int);
const int ADDR_sp_lux = ADDR_sp_sample_interval + sizeof(int);
const int ADDR_sp_dosing = ADDR_sp_lux + sizeof(float);
const int ADDR_sp_max_temp = ADDR_sp_dosing + sizeof(int);

// ============ HELPER FUNCTIONS ============
void setRelay(uint8_t pin, bool on){
  digitalWrite(pin, on ? HIGH : LOW);
}

void beep(int duration_ms) {
  digitalWrite(BUZZER_PIN, HIGH);
  isBuzzerOn = true;
  buzzerStartTime = millis();
}

void handleBuzzer() {
  if (isBuzzerOn) {
    if (millis() - buzzerStartTime >= 100) {
      digitalWrite(BUZZER_PIN, LOW);
      isBuzzerOn = false;
    }
  }
}

// Fungsi pengganti delay() biasa agar ESP32 tidak freeze/restart
void smartDelay(int duration_ms) {
  unsigned long start = millis();
  while (millis() - start < duration_ms) {
    esp_task_wdt_reset(); 
    handleBuzzer();
  }
}

float readStabilizedVoltage(int pin, int samples) {
  // Batasi sampel maksimal agar memori tidak penuh (Stack Overflow prevention)
  if (samples > 40) samples = 40; 
  int rawValues[40]; // Array statis (lebih aman daripada new/delete)

  // 1. Ambil Sampel
  for (int i = 0; i < samples; i++) {
    rawValues[i] = analogRead(pin);
    delay(3); // Beri jeda sedikit agar ADC tidak stress
  }

  // 2. Sorting (Urutkan dari Kecil ke Besar)
  for (int i = 0; i < samples - 1; i++) {
    for (int j = 0; j < samples - i - 1; j++) {
      if (rawValues[j] > rawValues[j + 1]) {
        int temp = rawValues[j];
        rawValues[j] = rawValues[j + 1];
        rawValues[j + 1] = temp;
      }
    }
  }

  // 3. Buang Data Ekstrem (Outlier)
  // Kita buang 30% data terbawah dan 30% data teratas (Spikes noise biasanya disini)
  long sum = 0;
  int count = 0;
  int startIndex = samples * 0.3; // Mulai dari indeks 30%
  int endIndex = samples * 0.7;   // Berhenti di indeks 70%

  for (int i = startIndex; i < endIndex; i++) {
    sum += rawValues[i];
    count++;
  }

  if (count == 0) count = 1; // Safety divide by zero
  float avgRaw = (float)sum / count;
  
  // Konversi ke Voltase
  uint32_t mv = esp_adc_cal_raw_to_voltage((int)avgRaw, adc_chars);
  return mv / 1000.0;
}

float ec_from_voltage(float V) {
  // HAPUS batas 0.30V. Kita izinkan tegangan rendah terbaca.
  // Tapi kita tetap butuh batas minimal noise (misal 0.02V)
  if (V <= 0.04) return 0.0; 
  return V * 4400.0; 
}

// ============ EEPROM HELPERS (OPTIMIZED) ============
void eepromWriteFloat(int addr, float val){ EEPROM.put(addr, val); }
float eepromReadFloat(int addr){ float v; EEPROM.get(addr, v); if(!isfinite(v) || v > 1e8 || v < -1e8) return 0.0; return v; }
void eepromWriteInt(int addr, int val){ EEPROM.put(addr, val); }
int eepromReadInt(int addr){ int v; EEPROM.get(addr, v); if(!isfinite(v) || v > 65000 || v < 0) return 0; return v; }

void saveAllToEEPROM(){
  eepromWriteFloat(ADDR_pH_offset, offset_pH);
  eepromWriteFloat(ADDR_EC_offset, offset_EC);
  eepromWriteFloat(ADDR_Kvalue, Kvalue);
  eepromWriteFloat(ADDR_slope, slope);
  eepromWriteFloat(ADDR_intercept, intercept);
  eepromWriteFloat(ADDR_setpoint_pH, setpointPH);
  eepromWriteFloat(ADDR_setpoint_EC, setpointEC);
  eepromWriteInt(ADDR_sp_interval, setpointSprayIntervalMenit);
  eepromWriteInt(ADDR_sp_duration, setpointSprayDurationDetik);
  eepromWriteInt(ADDR_sp_sample_interval, setpointSampleIntervalJam); 
  eepromWriteFloat(ADDR_sp_lux, setpointLux);
  eepromWriteInt(ADDR_sp_dosing, setpointDosingDur_ms);
  eepromWriteFloat(ADDR_sp_max_temp, setpointMaxTempWater);
  EEPROM.write(ADDR_MODE, modeAuto ? 1 : 0);
  EEPROM.commit();
}

void resetToFactoryDefaults() {
  setpointPH = DEFAULT_SP_PH; setpoint_pH = DEFAULT_SP_PH;
  setpointEC = DEFAULT_SP_EC; setpoint_EC_uS = DEFAULT_SP_EC;
  setpointSprayIntervalMenit = DEFAULT_SP_INT_SPRAY;
  setpointSprayDurationDetik = DEFAULT_SP_DUR_SPRAY;
  setpointSampleIntervalJam = DEFAULT_SP_INT_CHECK;
  setpointLux = DEFAULT_SP_LUX;
  setpointDosingDur_ms = DEFAULT_SP_DOSING;
  setpointMaxTempWater = DEFAULT_SP_MAX_TEMP;
  slope = -5.6; intercept = 15.5; Kvalue = 1.0;
  saveAllToEEPROM();
}

void loadAllFromEEPROM(){
  offset_pH = eepromReadFloat(ADDR_pH_offset);
  offset_EC = eepromReadFloat(ADDR_EC_offset);
  float k = eepromReadFloat(ADDR_Kvalue); if(isfinite(k) && k > 0.0 && k < 1e7) Kvalue = k; else Kvalue = 1.0;
  float s = eepromReadFloat(ADDR_slope); float itc = eepromReadFloat(ADDR_intercept); if(isfinite(s)) slope = s; if(isfinite(itc)) intercept = itc;
  float sp_pH = eepromReadFloat(ADDR_setpoint_pH); if(sp_pH > 0.0 && sp_pH < 15.0) { setpoint_pH = sp_pH; setpointPH = sp_pH; }
  float sp_ec = eepromReadFloat(ADDR_setpoint_EC); if(sp_ec > 0.0 && sp_ec < 20000.0) { setpointEC = sp_ec; setpoint_EC_uS = sp_ec; }
  int sp_int = eepromReadInt(ADDR_sp_interval); if(sp_int > 0 && sp_int < 1000) setpointSprayIntervalMenit = sp_int;
  int sp_dur = eepromReadInt(ADDR_sp_duration); if(sp_dur > 0 && sp_dur < 1000) setpointSprayDurationDetik = sp_dur;
  int sp_samp_int = eepromReadInt(ADDR_sp_sample_interval); if(sp_samp_int > 0 && sp_samp_int < 100) setpointSampleIntervalJam = sp_samp_int;
  float sp_lux = eepromReadFloat(ADDR_sp_lux); if(sp_lux > 0.0 && sp_lux < 65000.0) setpointLux = sp_lux;
  int sp_dose = eepromReadInt(ADDR_sp_dosing); if(sp_dose > 100 && sp_dose < 20000) setpointDosingDur_ms = sp_dose;
  float sp_mtemp = eepromReadFloat(ADDR_sp_max_temp); if(sp_mtemp > 10.0 && sp_mtemp < 60.0) setpointMaxTempWater = sp_mtemp;
  modeAuto = (EEPROM.read(ADDR_MODE) == 1);
}

// Safety Check Sensor
bool isSensorValid(float valPH, float valEC) {
  if (isnan(valPH) || valPH < 2.5 || valPH > 10.0) return false; 
  if (isnan(valEC) || valEC < 50.0) return false;
  return true; 
}

// ============ LOGIC FUNCTIONS ============
float getTemperature(){
  DS18B20.requestTemperatures();
  float t = DS18B20.getTempCByIndex(0);
  if(t == DEVICE_DISCONNECTED_C) return sensor_temp;
  return t;
}

void updateSensors(){
  unsigned long now = millis();
  
  if (now - lastDSRequest >= DS_DELAY_MS) {
    float t = DS18B20.getTempCByIndex(0);
    if (t != DEVICE_DISCONNECTED_C && t != 85.0) sensor_temp = t; 
    DS18B20.requestTemperatures(); lastDSRequest = now;
    
    float luxval = lightSensor.readLightLevel();
    if(luxval >= 0) sensor_lux = luxval;
  }

  static unsigned long lastDHTRead = 0;
  if (now - lastDHTRead >= 2000) { 
    lastDHTRead = now;
    float t_dht = dht.readTemperature(); 
    float h_dht = dht.readHumidity();
    if(!isnan(t_dht)) sensor_temp_dht = t_dht; 
    if(!isnan(h_dht)) sensor_hum_dht = h_dht;
  }

  float v_ph = readStabilizedVoltage(PH_PIN, 30);
  float ph_raw = (slope == 0.0 && intercept == 0.0) ? 7.0 : (slope * v_ph + intercept);
  sensor_pH = ph_raw; if(!isfinite(sensor_pH)) sensor_pH = 0.0;

  float V_ec = readStabilizedVoltage(EC_PIN, 30);
  float ec_raw = ec_from_voltage(V_ec); 
  float ec_cal = ec_raw * Kvalue;
  float ec25 = ec_cal / (1.0 + 0.02 * (sensor_temp - 25.0));
  sensor_EC_uScm = ec25; if(!isfinite(sensor_EC_uScm)) sensor_EC_uScm = 0.0;

  EC = sensor_EC_uScm; pH = sensor_pH; tempAir = sensor_temp;
  tempUdara = sensor_temp_dht; humUdara = sensor_hum_dht; lux = sensor_lux;
}

bool evaluateSamplingReady(float measuredEC, float measuredPH) {
  if(measuredEC <= 0) return false;
  //toleransi nutrisi
  float ecLower = setpoint_EC_uS * 0.75; float ecUpper = setpoint_EC_uS * 1.25;
  bool ecOK = (measuredEC >= ecLower && measuredEC <= ecUpper);
  bool pHOK = (measuredPH >= (setpoint_pH - 0.6) && measuredPH <= (setpoint_pH + 0.6));
  return (ecOK && pHOK);
}

// ============ UI DISPLAY & CALIB LOGIC ============
void showModeMenu() {
  lcd.clear(); lcd.setCursor(0,0); lcd.print(F("== Pilih Mode =="));
  if (cursorInAutoMode) { 
    lcd.setCursor(0,1); lcd.print(F("> Mode Auto")); 
    lcd.setCursor(0,2); lcd.print(F("  Mode Manual")); 
  } else { 
    lcd.setCursor(0,1); lcd.print(F("  Mode Auto")); 
    lcd.setCursor(0,2); lcd.print(F("> Mode Manual")); 
  }
  lcd.setCursor(0,3); lcd.print(F("R/L:Geser | E:OK"));
}

void showMonitoring() {
  lcd.setCursor(1,0); lcd.print(F("Monitoring System "));
  lcd.setCursor(0,1); lcd.print(F("EC:")); lcd.print((int)EC); lcd.print(F(" uS   ")); 
  lcd.setCursor(11,1); lcd.print(F("Ta:")); lcd.print(tempAir,1); lcd.print(F("C"));
  lcd.setCursor(0,2); lcd.print(F("pH:")); lcd.print(pH,2); lcd.print(F("    "));
  lcd.setCursor(11,2); lcd.print(F("Lux:")); lcd.print((int)lux); 
  lcd.setCursor(0,3); lcd.print(F("T:")); lcd.print(tempUdara,1); lcd.print(F("C"));
  lcd.setCursor(11,3); lcd.print(F("H:")); lcd.print(humUdara,0); lcd.print(F("%"));
} 

void samplingRoutine() {
  retryCount = 0; lcd.clear();
  lastSamplingReady = false;
  lcd.setCursor(0,0); lcd.print(F("Sampling Nutrisi..."));
  lcd.setCursor(0,1); lcd.print(F("1. Mixing Awal...")); // Ubah teks
  
  // Matikan semua dulu
  setRelay(RELAY_NUTRI_AB_PIN, false); setRelay(RELAY_MIXER_PIN, false);
  setRelay(RELAY_SAMPLING_PIN, false); setRelay(RELAY_BUANG_PIN, false);
  
  // LANGSUNG KE MIXING (Jangan nyalakan Nutri Pump dulu!)
  setRelay(RELAY_MIXER_PIN, true); 
  processStartTime = millis(); 
  currentProcess = PROC_SAMPLING_MIXING; // Ubah state awal ke Mixing
}

void showMainMenu() {
  lcd.clear(); lcd.setCursor(2,0); lcd.print(F("== Mode MANUAL =="));
  String item;
  switch (mainIndex) {
    case 0: item = "Monitoring"; break;
    case 1: item = "Sampling Nutrisi"; break;
    case 2: item = "Siram / Spray"; break;
    case 3: item = "Setting"; break;
  }
  lcd.setCursor(0,1); lcd.print("> " + item); 
  lcd.setCursor(0,3); lcd.print(F("E:Pilih | R/L:Geser"));
}

void sprayRoutine() {
  // --- LOGIKA PERBAIKAN ---
  
  // 1. Cek Mode AUTO:
  // Kalau Mode Auto, kita harus ketat. Cek "Sertifikat" (lastSamplingReady).
  if (modeAuto && !lastSamplingReady) {
      lcd.clear(); lcd.setCursor(0,1); lcd.print(F("Auto-Spray Gagal!"));
      lcd.setCursor(0,2); lcd.print(F("Nutrisi Belum Siap"));
      delay(2000); showModeMenu(); return;
  }

  // 2. Cek Mode MANUAL:
  // Kalau Manual, kita izinkan ASALKAN bukan kondisi darurat hardware.
  // Kita ABAIKAN pembacaan sensor EC/pH saat ini (karena mungkin wadah kosong).
  // Jadi meskipun EC terbaca 0 karena kering, Spray tetap jalan.
  
  // --- EKSEKUSI SPRAY ---
  lcd.clear(); lcd.setCursor(0,0); lcd.print(F("Spray ON..."));
  lcd.setCursor(0,1); lcd.print(F("Durasi: ")); lcd.print(setpointSprayDurationDetik); lcd.print(F(" dtk"));
  
  // Tampilkan data sampling TERAKHIR (Bukan data sensor kering saat ini)
  lcd.setCursor(0,2); lcd.print(F("Last EC:")); lcd.print((int)lastSamplingEC);
  lcd.setCursor(0,3); lcd.print(F("Last pH:")); lcd.print(lastSamplingPH, 1);

  setRelay(RELAY_SPRAY_PIN, true);
  processStartTime = millis(); 
  currentProcess = PROC_SPRAYING;
}

void showSettingMenu() {
  lcd.clear(); lcd.setCursor(0,0); lcd.print(F("== Setting Menu =="));
  String items[] = {"Kalibrasi", "SetPoint", "Manual Test", "Atur Jam/RTC", "Factory Reset"};
  int start = (settingIndex > 2) ? settingIndex - 2 : 0;
  for(int i=0; i<3; i++) {
     int idx = start + i;
     if(idx >= 5) break;
     lcd.setCursor(0, i+1);
     lcd.print((settingIndex == idx) ? "> " : "  ");
     lcd.print(items[idx]);
  }
}

void showCalibMenu() {
  lcd.clear(); lcd.setCursor(0,0); lcd.print(F("== Kalibrasi =="));
  lcd.setCursor(0,1); lcd.print((calIndex==0)?"> EC Sensor":"  EC Sensor");
  lcd.setCursor(0,2); lcd.print((calIndex==1)?"> pH Sensor":"  pH Sensor");
  lcd.setCursor(0,3); lcd.print(F("R/L:Gulir E:Pilih"));
}

void showSetpointMenu() {
  lcd.clear(); lcd.setCursor(0, 0); lcd.print(F("== SetPoint =="));
  String items[] = { "EC", "pH", "Spray Int (m)", "Spray Dur(s)", "Cek Int (j)", 
                       "Lux Target", "DosePump pH(ms)", "Max Temp Air" };
  int totalItems = SETPOINT_MENU_ITEMS; int startIndex = (spIndex / 3) * 3;
  for (int i = 0; i < 3; i++) {
    int itemIndex = startIndex + i;
    if (itemIndex >= totalItems) break;
    lcd.setCursor(0, i + 1); lcd.print((itemIndex == spIndex) ? ">" : " "); lcd.print(items[itemIndex]);
    lcd.setCursor(13, i + 1);
    if (itemIndex == 0) lcd.print((int)setpointEC);
    else if (itemIndex == 1) lcd.print(setpointPH, 1);
    else if (itemIndex == 2) lcd.print(setpointSprayIntervalMenit);
    else if (itemIndex == 3) lcd.print(setpointSprayDurationDetik);
    else if (itemIndex == 4) lcd.print(setpointSampleIntervalJam);
    else if (itemIndex == 5) lcd.print((int)setpointLux);        
    else if (itemIndex == 6) lcd.print(setpointDosingDur_ms);    
    else if (itemIndex == 7) lcd.print(setpointMaxTempWater, 1); 
  }
  lcd.setCursor(18, 0); if (startIndex > 0) lcd.print("up");
  lcd.setCursor(16, 3); if (startIndex + 3 < totalItems) lcd.print("down");
}

void showManualTestMenu(bool forceClear = true) {
  if (forceClear) {
    lcd.clear(); 
    lcd.setCursor(0,0); lcd.print(F("== Manual Test =="));
  }
  String items[] = {"Spray", "Mixer", "Nutri AB", "Sampling", "Buang", "pH Up", "pH Down", "Lampu"};
  bool status[] = {
    digitalRead(RELAY_SPRAY_PIN), digitalRead(RELAY_MIXER_PIN), 
    digitalRead(RELAY_NUTRI_AB_PIN), digitalRead(RELAY_SAMPLING_PIN), 
    digitalRead(RELAY_BUANG_PIN), digitalRead(RELAY_PH_UP_PIN),
    digitalRead(RELAY_PH_DOWN_PIN), digitalRead(RELAY_LAMPU_PIN)
  };
  int start = (manualIndex > 2) ? manualIndex - 2 : 0;
  for(int i=0; i<3; i++) {
     int idx = start + i;
     if(idx >= MANUAL_ITEMS) break;
     if (forceClear) {
       lcd.setCursor(0, i+1);
       lcd.print((manualIndex == idx) ? "> " : "  ");
       lcd.print(items[idx]);
     }
     lcd.setCursor(13, i+1);
     lcd.print(status[idx] ? F("[ON] ") : F("[OFF]"));
  }
}

void showRTCMenu() {
  lcd.clear(); lcd.setCursor(0,0); lcd.print(F("== Atur Waktu =="));
  char buf[20];
  sprintf(buf, "Jam:   %02d:%02d", tempHour, tempMin);
  lcd.setCursor(0,1); lcd.print(buf);
  lcd.setCursor(0,2);
  if (rtcEditState == 0) lcd.print(F("       ^^      ")); 
  else lcd.print(F("          ^^   ")); 
  lcd.setCursor(0,3); lcd.print(F("R/L:Ubah E:Ok B:Exit"));
}

void enterMainOption() {
  if (mainIndex == 0) showMonitoring(); 
  else if (mainIndex == 1) samplingRoutine();
  else if (mainIndex == 2) sprayRoutine();
  else if (mainIndex == 3) { currentLevel = SETTING_MENU; showSettingMenu(); }
}

void enterSettingOption() {
  if (settingIndex == 0) { currentLevel = CALIB_MENU; showCalibMenu(); }
  else if (settingIndex == 1) { currentLevel = SETPOINT_MENU; showSetpointMenu(); }
  else if (settingIndex == 2) { 
       currentLevel = MANUAL_TEST_MENU; 
       manualIndex = 0; 
       showManualTestMenu(); 
  }
  else if (settingIndex == 3) { // RTC Menu
       currentLevel = RTC_MENU;
       DateTime now = rtc.now();
       tempHour = now.hour();
       tempMin = now.minute();
       rtcEditState = 0;
       showRTCMenu();
  }
  else if (settingIndex == 4) { // Factory Reset
       currentLevel = FACTORY_RESET_MENU;
       inFactoryResetConfirm = true;
       lcd.clear(); lcd.setCursor(0,0); lcd.print(F("== Factory Reset =="));
       lcd.setCursor(0,1); lcd.print(F("Semua data hilang!"));
       lcd.setCursor(0,2); lcd.print(F("Yakin Reset?"));
       lcd.setCursor(0,3); lcd.print(F("E:Ya(Reset) B:Batal"));
  }
}

void showCurrentDigit() {
  lcd.setCursor(0,1); lcd.print(F("Nilai: "));
  int len = strlen(digitsBuf);
  for (int i=0; i<len; i++) {
    if (i == currentDigitIndex) lcd.print('[');
    lcd.print(digitsBuf[i]);
    if (i == currentDigitIndex) lcd.print(']');
  }
  for (int i=len*2+7; i<20; i++) lcd.print(' ');
  lcd.setCursor(0,3); lcd.print(F("R/L:Ubah  E:Next"));
}

void startSetpointAdjust(String type) {
  inSetpointAdjust = true; currentType = type; currentDigitIndex = 0; waitingForBackAfterSave = false;
  float initialValue = 0.0; int precision = 1;
  if (type == "EC") { initialValue = setpointEC; precision = 0; }
  else if (type == "pH") { initialValue = setpointPH; precision = 1; }
  else if (type == "Spray Int (m)") { initialValue = setpointSprayIntervalMenit; precision = 0; }
  else if (type == "Spray Dur(s)") { initialValue = setpointSprayDurationDetik; precision = 0; }
  else if (type == "Cek Int (j)") { initialValue = setpointSampleIntervalJam; precision = 0; }
  else if (type == "Lux Target") { initialValue = setpointLux; precision = 0; }        
  else if (type == "DosePump pH(ms)") { initialValue = setpointDosingDur_ms; precision = 0; } 
  else if (type == "Max Temp Air") { initialValue = setpointMaxTempWater; precision = 1; } 

  char format[5];
  if (precision == 0) { sprintf(format, "%4d"); sprintf(digitsBuf, format, (int)initialValue); }
  else { dtostrf(initialValue, 4, 1, digitsBuf); }
  for(int i=0; i<4; i++) { if(digitsBuf[i] == ' ') digitsBuf[i] = ' '; }
  digitsBuf[4] = '\0'; 
  lcd.clear(); lcd.setCursor(0,0); lcd.print("Atur " + type); showCurrentDigit();
}

void showConfirmation() {
  String valStr = String(digitsBuf);
  lcd.clear(); lcd.setCursor(0, 0); lcd.print(F("Simpan nilai ini?"));
  lcd.setCursor(0, 1); lcd.print(currentType + ": " + valStr);
  lcd.setCursor(0, 3); lcd.print(F("E:Simpan | B:Batal"));
  currentDigitIndex = 99;
}

int getNextCharIndex(char c, bool dirRight) {
  int idx = 0;
  for (int i=0; i<11; i++) if (digitList[i]==c) idx=i;
  if (dirRight) idx = (idx+1)%11; 
  else idx = (idx==0?10:idx-1);
  return idx;
}

void handleSetpointInput(char cmd) {
  if (waitingForBackAfterSave) {
    if (cmd == 'B') { waitingForBackAfterSave = false; inSetpointAdjust = false; showSetpointMenu(); }
    return;
  }
  int len = strlen(digitsBuf);
  if (currentDigitIndex == 99) { 
    if (cmd == 'E') {
      String s = String(digitsBuf); s.trim(); float val_f = s.toFloat(); int val_i = s.toInt();
      if (currentType == "EC") { setpointEC = val_f; setpoint_EC_uS = val_f; }
      else if (currentType == "pH") { setpointPH = val_f; setpoint_pH = val_f; }
      else if (currentType == "Spray Int (m)") { setpointSprayIntervalMenit = val_i; }
      else if (currentType == "Spray Dur(s)") { setpointSprayDurationDetik = val_i; }
      else if (currentType == "Cek Int (j)") { setpointSampleIntervalJam = val_i; }
      else if (currentType == "Lux Target") { setpointLux = val_f; }              
      else if (currentType == "DosePump pH(ms)") { setpointDosingDur_ms = val_i; } 
      else if (currentType == "Max Temp Air") { setpointMaxTempWater = val_f; }  
      
      saveAllToEEPROM();
      if (WiFi.status() == WL_CONNECTED && signupOK) {
          FirebaseJson jsonSP;
          jsonSP.set("target_ec", setpointEC);
          jsonSP.set("target_ph", setpointPH);
          jsonSP.set("spray_interval", setpointSprayIntervalMenit);
          jsonSP.set("spray_duration", setpointSprayDurationDetik);
          jsonSP.set("check_interval", setpointSampleIntervalJam);
          jsonSP.set("lux_target", setpointLux);
          jsonSP.set("dose_duration", setpointDosingDur_ms);
          jsonSP.set("max_temp", setpointMaxTempWater);
          
          // Kirim ke path "/setpoints"
          Firebase.RTDB.updateNode(&fbdo, "/setpoints", &jsonSP);
      }
      lcd.clear(); lcd.setCursor(0, 1); lcd.print(F("SetPoint Disimpan!"));
      lcd.setCursor(0, 3); lcd.print(F("Tekan BACK u/ kembali"));
      waitingForBackAfterSave = true;
    } else if (cmd == 'B') { inSetpointAdjust = false; showSetpointMenu(); }
    return;
  }
  
  if (cmd == 'R') {
    char cur = digitsBuf[currentDigitIndex]; if(cur == ' ') cur = '0';
    int idx = getNextCharIndex(cur, true); digitsBuf[currentDigitIndex] = digitList[idx];
  } else if (cmd == 'L') {
    char cur = digitsBuf[currentDigitIndex]; if(cur == ' ') cur = '.';
    int idx = getNextCharIndex(cur, false); digitsBuf[currentDigitIndex] = digitList[idx];
  } else if (cmd == 'E') {
    if (currentDigitIndex < len - 1) currentDigitIndex++; else showConfirmation();
  } else if (cmd == 'B') {
    if (currentDigitIndex > 0) { currentDigitIndex--; } 
    else { inSetpointAdjust = false; showSetpointMenu(); return; }
  }
  if (currentDigitIndex != 99) showCurrentDigit();
}

void startCalibration(String type, String A, String B) {
  inCalibration = true; calibrationStep = 1; calType = type; 
  if(type == "pH") { calA = "7.0"; calB = "4.0"; } 
  else { calA = A; calB = B; } 
  isCalibSampling = false; tempFixedVoltage = 0.0;
}

void runCalibrationDisplay() {
  updateSensors(); 
  static unsigned long lastCalibLCDUpdate = 0;
  if (millis() - lastCalibLCDUpdate < 250) return; 
  lastCalibLCDUpdate = millis();

  lcd.setCursor(0, 0); 
  lcd.print("Calib " + calType + " Step " + String(calibrationStep) + "   ");

  if (isCalibSampling) {
    unsigned long elapsed = millis() - calibStartTime;
    lcd.setCursor(0, 1); lcd.print(F("Membaca Sensor...   "));
    lcd.setCursor(0, 2); 
    int progress = map(elapsed, 0, CALIB_DURATION_MS, 0, 20);
    String bar = ""; for(int i=0; i<progress; i++) bar += ">";
    while(bar.length() < 20) bar += " "; lcd.print(bar);
    float currentV = 0.0;
    if (calType == "pH") currentV = readStabilizedVoltage(PH_PIN, 5); else currentV = readStabilizedVoltage(EC_PIN, 5);
    calVoltageAccumulator += currentV; calSampleCount++;
    if (elapsed >= CALIB_DURATION_MS) {
      isCalibSampling = false; 
      if (calSampleCount > 0) tempFixedVoltage = calVoltageAccumulator / calSampleCount; 
      else tempFixedVoltage = 0;
      beep(500); lcd.setCursor(0, 2); lcd.print(F("                    ")); 
    }
    return; 
  }

  if (calibrationStep == 1) {
    lcd.setCursor(0, 1); 
    if (calType == "pH") lcd.print("Celup ke: " + calA + "    "); 
    else lcd.print(F("Probe Kering/Udara  ")); 

    if (tempFixedVoltage > 0.1 || (calType == "EC" && tempFixedVoltage >= 0.0)) { 
      lcd.setCursor(0, 2); lcd.print(F("Terbaca: ")); 
      if(calType == "pH") { lcd.print(tempFixedVoltage, 3); lcd.print(F("V       ")); } 
      else { float ec_est = ec_from_voltage(tempFixedVoltage); lcd.print((int)ec_est); lcd.print(F(" uS(Nol)")); }
      lcd.setCursor(0, 3); lcd.print(F("E:Lanjut Step 2     "));
    } else {
      lcd.setCursor(0, 2); lcd.print(F("Live: ")); 
      if(calType == "pH") { lcd.print(readStabilizedVoltage(PH_PIN, 10), 4); lcd.print(F(" V         ")); } 
      else { float v_now = readStabilizedVoltage(EC_PIN, 10); float ec_now = ec_from_voltage(v_now); lcd.print((int)ec_now); lcd.print(F(" uS        ")); }
      lcd.setCursor(0, 3); lcd.print(F("E:Mulai Sampling    "));
    }
  } else if (calibrationStep == 2) {
    lcd.setCursor(0, 1); lcd.print(F("Angkat & Cuci Air   "));
    lcd.setCursor(0, 2); lcd.print("Lalu celup " + calB + "    ");
    lcd.setCursor(0, 3); lcd.print(F("E:Siap Step Akhir   "));
  } else if (calibrationStep == 3) {
    lcd.setCursor(0, 1); lcd.print("Celup ke: " + calB + "    ");
    if (tempFixedVoltage > 0.1) {
       lcd.setCursor(0, 2); lcd.print(F("Terbaca: ")); lcd.print(tempFixedVoltage, 3); lcd.print(F("V       "));
       lcd.setCursor(0, 3); lcd.print(F("E:SIMPAN DATA       "));
    } else {
       lcd.setCursor(0, 2); lcd.print(F("Live: ")); 
       if(calType == "pH") { lcd.print(readStabilizedVoltage(PH_PIN, 10), 4); lcd.print(F(" V         ")); } 
       else { float v_now = readStabilizedVoltage(EC_PIN, 10); float ec_now = ec_from_voltage(v_now); lcd.print((int)ec_now); lcd.print(F(" uS        ")); }
       lcd.setCursor(0, 3); lcd.print(F("E:Mulai Sampling    "));
    }
  }
}

void nextCalibrationStep() {
  beep(100);
  if (calibrationStep == 1) {
    if (tempFixedVoltage < 0.1 && !(calType=="EC" && tempFixedVoltage >= 0.0)) {
      isCalibSampling = true; calibStartTime = millis();
      calVoltageAccumulator = 0; calSampleCount = 0;
    } else {
      calVoltage_Step1 = tempFixedVoltage; 
      tempFixedVoltage = 0.0; calibrationStep = 2; 
    }
  } else if (calibrationStep == 2) {
      tempFixedVoltage = 0.0; calibrationStep = 3; 
  } else if (calibrationStep == 3) {
    if (tempFixedVoltage < 0.1) {
      isCalibSampling = true; calibStartTime = millis();
      calVoltageAccumulator = 0; calSampleCount = 0;
    } else {
      float calVoltage_Step2 = tempFixedVoltage; 
      if (calType == "pH") {
          float v7 = calVoltage_Step1; float v4 = calVoltage_Step2; 
          slope = (6.86 - 4.0) / (v7 - v4); 
          intercept = 6.86 - (slope * v7);
          eepromWriteFloat(ADDR_slope, slope); 
          eepromWriteFloat(ADDR_intercept, intercept);
      } else {
          float ec_raw = ec_from_voltage(calVoltage_Step2);
          if(ec_raw > 0) {
              float target_EC = calB.toFloat(); 
              float temp = getTemperature();
              float target_at_temp = target_EC * (1.0 + 0.02 * (temp - 25.0));
              Kvalue = target_at_temp / ec_raw;
              eepromWriteFloat(ADDR_Kvalue, Kvalue);
          }
      }
      EEPROM.commit(); 
      lcd.clear(); lcd.setCursor(0,0); lcd.print(F("KALIBRASI SUKSES!"));
      lcd.setCursor(0,1); lcd.print(F("Disimpan ke Memori"));
      if (calType == "pH") { lcd.setCursor(0,2); lcd.print(F("Slope: ")); lcd.print(slope, 1); } 
      else { lcd.setCursor(0,2); lcd.print(F("K-Val: ")); lcd.print(Kvalue, 2); }
      lastFirebaseSync = millis(); 
      lastInputTime = millis(); 
      smartDelay(2000);
      inCalibration = false; calibrationStep = 0; tempFixedVoltage = 0; showCalibMenu();
    }
  }
}

void cancelCalibration() {
  inCalibration = false; calibrationStep = 0; showCalibMenu();
}

void handleSerial(){
  while(Serial.available()){
    char c = (char)Serial.read(); if(c == '\r') continue;
    if(c == '\n'){ inputLine = ""; } else { inputLine += c; }
  }
}

void handleInput(char cmd){
  beep(100);
  if (waitingForAckAfterSampling) {
    if (cmd == 'E' || cmd == 'B') { waitingForAckAfterSampling = false; showMainMenu(); }
    return; 
  }
  lastInputTime = millis(); autoLoopActive = false;
  if (inCalibration) {
    if (cmd == 'E') nextCalibrationStep(); else if (cmd == 'B') cancelCalibration();
    return;
  }
  if (inSetpointAdjust) { handleSetpointInput(cmd); return; }

  switch (currentLevel) {
    case MODE_MENU:
      if (cmd == 'R' || cmd == 'L') { cursorInAutoMode = !cursorInAutoMode; showModeMenu(); }
      else if (cmd == 'E') {
        modeAuto = cursorInAutoMode; 
        EEPROM.begin(512); EEPROM.write(ADDR_MODE, modeAuto ? 1 : 0); EEPROM.commit(); EEPROM.end();
        if (modeAuto) { lcd.clear(); lcd.setCursor(0,1); lcd.print(F("Mode AUTO Aktif...")); delay(50); currentLevel = MAIN_MENU; mainIndex = 0; showMainMenu(); } 
        else { lcd.clear(); lcd.setCursor(0,1); lcd.print(F("Mode MANUAL...")); delay(500); currentLevel = MAIN_MENU; mainIndex = 0; showMainMenu(); }
      }
      break;
    case MAIN_MENU:
      if (cmd == 'R') { mainIndex = (mainIndex + 1) % MAIN_MENU_ITEMS; showMainMenu(); }
      else if (cmd == 'L') { mainIndex = (mainIndex + MAIN_MENU_ITEMS - 1) % MAIN_MENU_ITEMS; showMainMenu(); }
      else if (cmd == 'E') enterMainOption();
      else if (cmd == 'B') { currentLevel = MODE_MENU; showModeMenu(); }
      break;
    case SETTING_MENU:
      if (cmd == 'R') { settingIndex = (settingIndex + 1) % SETTING_MENU_ITEMS; showSettingMenu(); }
      else if (cmd == 'L') { settingIndex = (settingIndex + SETTING_MENU_ITEMS - 1) % SETTING_MENU_ITEMS; showSettingMenu(); }
      else if (cmd == 'E') enterSettingOption();
      else if (cmd == 'B') { currentLevel = MAIN_MENU; showMainMenu(); }
      break;
    case CALIB_MENU:
      if (cmd == 'R') { calIndex = (calIndex + 1) % CALIB_MENU_ITEMS; showCalibMenu(); }
      else if (cmd == 'L') { calIndex = (calIndex + CALIB_MENU_ITEMS - 1) % CALIB_MENU_ITEMS; showCalibMenu(); }
      else if (cmd == 'E') {
        if (calIndex == 0) startCalibration("EC", String((int)ecCalA_val), String((int)ecCalB_val));
        else startCalibration("pH", "6.86", "4.0");
      }
      else if (cmd == 'B') { currentLevel = SETTING_MENU; showSettingMenu(); }
      break;
    case SETPOINT_MENU: {
      if (cmd == 'R') { spIndex = (spIndex + 1) % SETPOINT_MENU_ITEMS; showSetpointMenu(); }
      else if (cmd == 'L') { spIndex = (spIndex + SETPOINT_MENU_ITEMS - 1) % SETPOINT_MENU_ITEMS; showSetpointMenu(); }
      else if (cmd == 'E') {
        String items[] = {"EC", "pH", "Spray Int (m)", "Spray Dur(s)", "Cek Int (j)", "Lux Target", "DosePump pH(ms)", "Max Temp Air"};
        startSetpointAdjust(items[spIndex]); 
      }
      else if (cmd == 'B') { currentLevel = SETTING_MENU; showSettingMenu(); }
      break;
    }
    case MANUAL_TEST_MENU: {
      if (cmd == 'R') { manualIndex = (manualIndex + 1) % MANUAL_ITEMS; showManualTestMenu(true); }
      else if (cmd == 'L') { manualIndex = (manualIndex + MANUAL_ITEMS - 1) % MANUAL_ITEMS; showManualTestMenu(true); }
      else if (cmd == 'E') {
         int pin = 0;
         if(manualIndex==0) pin = RELAY_SPRAY_PIN;
         else if(manualIndex==1) pin = RELAY_MIXER_PIN;
         else if(manualIndex==2) pin = RELAY_NUTRI_AB_PIN;
         else if(manualIndex==3) pin = RELAY_SAMPLING_PIN;
         else if(manualIndex==4) pin = RELAY_BUANG_PIN;
         else if(manualIndex==5) pin = RELAY_PH_UP_PIN;
         else if(manualIndex==6) pin = RELAY_PH_DOWN_PIN;
         else if(manualIndex==7) pin = RELAY_LAMPU_PIN;
         setRelay(pin, !digitalRead(pin)); 
         showManualTestMenu(false);
      }
      else if (cmd == 'B') { 
         // SAFETY: Matikan semua relay saat keluar
         setRelay(RELAY_SPRAY_PIN, false); setRelay(RELAY_MIXER_PIN, false);
         setRelay(RELAY_NUTRI_AB_PIN, false); setRelay(RELAY_SAMPLING_PIN, false);
         setRelay(RELAY_BUANG_PIN, false); setRelay(RELAY_PH_UP_PIN, false);
         setRelay(RELAY_PH_DOWN_PIN, false); setRelay(RELAY_LAMPU_PIN, false);
         currentLevel = SETTING_MENU; showSettingMenu(); 
      }
      break;
    }
    case RTC_MENU: {
      if (cmd == 'R') { 
         if (rtcEditState == 0) tempHour = (tempHour + 1) % 24; 
         else tempMin = (tempMin + 1) % 60;
         showRTCMenu();
      }
      else if (cmd == 'L') { 
         if (rtcEditState == 0) tempHour = (tempHour + 24 - 1) % 24; 
         else tempMin = (tempMin + 60 - 1) % 60;
         showRTCMenu();
      }
      else if (cmd == 'E') {
         if (rtcEditState == 0) { rtcEditState = 1; showRTCMenu(); } 
         else {
           DateTime now = rtc.now();
            uint16_t setYear = now.year();
            uint8_t setMonth = now.month();
            uint8_t setDay = now.day();
            if (setYear < 2025) { setYear = 2025; setMonth = 1; setDay = 1; }
            rtc.adjust(DateTime(setYear, setMonth, setDay, tempHour, tempMin, 0));
           lcd.clear(); lcd.setCursor(0,1); lcd.print(F("Waktu Tersimpan!"));
           delay(1500);
           currentLevel = SETTING_MENU; showSettingMenu();
         }
      }
      else if (cmd == 'B') { 
         if (rtcEditState == 1) { rtcEditState = 0; showRTCMenu(); }
         else { currentLevel = SETTING_MENU; showSettingMenu(); }
      }
      break;
    }
    case FACTORY_RESET_MENU: {
      if (cmd == 'E') {
        lcd.clear(); lcd.print(F("Resetting..."));
        resetToFactoryDefaults();
        delay(2000); lcd.setCursor(0,1); lcd.print(F("Selesai!")); delay(1000);
        currentLevel = SETTING_MENU; showSettingMenu();
      } else if (cmd == 'B') {
        currentLevel = SETTING_MENU; showSettingMenu();
      }
      break;
    }
  }
}

void handleEmergencyConfirm() {
  setRelay(RELAY_NUTRI_AB_PIN, false); setRelay(RELAY_MIXER_PIN, false);
  setRelay(RELAY_SAMPLING_PIN, false); setRelay(RELAY_BUANG_PIN, false);
  setRelay(RELAY_PH_UP_PIN, false);    setRelay(RELAY_PH_DOWN_PIN, false);
  setRelay(RELAY_SPRAY_PIN, false);
  
  lcd.clear(); lcd.setCursor(0, 0); lcd.print(F("!! PAUSE PROSES !!"));
  lcd.setCursor(0, 1); lcd.print(F("Yakin Stop Proses?"));
  lcd.setCursor(0, 2); lcd.print(F("ENTER : YA (Stop)")); 
  lcd.setCursor(0, 3); lcd.print(F("BACK  : TIDAK (Ljt)"));
  beep(100); delay(100); beep(100); 
  
  while (true) {
    esp_task_wdt_reset(); // WATCHDOG RESET (PENTING)
    handleBuzzer();        
    
    if (digitalRead(BTN_ENTER_PIN) == LOW) {
       beep(500); currentProcess = PROC_IDLE; processStartTime = 0; retryCount = 0;
       waitingForAckAfterSampling = false;
       lcd.clear(); lcd.setCursor(0, 1); lcd.print(F("PROSES DIHENTIKAN!")); 
       delay(2000); showMainMenu(); return; 
    }
    if (digitalRead(BTN_BACK_PIN) == LOW) {
       while(digitalRead(BTN_BACK_PIN) == LOW) { delay(10); esp_task_wdt_reset(); }
       lcd.clear(); lcd.setCursor(0, 1); lcd.print(F("Melanjutkan...")); delay(1000); return; 
    }
    delay(50);
  }
}

void handleAutoLoop() {
  unsigned long durasiTampil = 4000; 
  if (loopPage == 1) durasiTampil = 10000; else if (loopPage == 2) durasiTampil = 6000;

  if (millis() - lastLoopChange > durasiTampil) {
    lastLoopChange = millis(); loopPage = (loopPage + 1) % 3; lcd.clear(); 
  }

  static unsigned long lastAutoRefresh = 0;
  if (millis() - lastAutoRefresh > 500) { 
    lastAutoRefresh = millis();
    if (currentLevel == MODE_MENU || currentLevel == MAIN_MENU) {
        if (loopPage == 0) {
            lcd.setCursor(5,1); lcd.print(F("Aeroponik")); 
            lcd.setCursor(3,2); lcd.print(F("Flos x UNSOED"));
        } 
        else if (loopPage == 1) {
            updateSensors(); 
            showMonitoring(); // Sudah dioptimalkan
        }
        else if (loopPage == 2) {
            lcd.setCursor(0,0); 
            if(modeAuto) lcd.print(F("Mode: AUTO       ")); 
            else lcd.print(F("Mode: MANUAL     "));
            
            DateTime now = rtc.now();
            int sisaMenitSpray = 0;
            if(setpointSprayIntervalMenit > 0) sisaMenitSpray = setpointSprayIntervalMenit - (now.minute() % setpointSprayIntervalMenit);
            
            lcd.setCursor(0,1); lcd.print(F("Next Spray: ")); lcd.print(sisaMenitSpray); lcd.print(F(" m "));
            
            int sisaJamCek = 0;
            if(setpointSampleIntervalJam > 0) sisaJamCek = setpointSampleIntervalJam - (now.hour() % setpointSampleIntervalJam);
            
            lcd.setCursor(0,2); lcd.print(F("Next CekNut:")); lcd.print(sisaJamCek); lcd.print(F("j "));
            
            char timeBuf[6]; sprintf(timeBuf, "%02d:%02d", now.hour(), now.minute());
            lcd.setCursor(0,3); lcd.print(F("Jam: ")); lcd.print(timeBuf);
        }
    }
  }
}

// ============ MAIN STATE MACHINE ============
void handleProcesses() {
  if (currentProcess == PROC_IDLE) return;
  unsigned long elapsedTime = millis() - processStartTime;

  switch (currentProcess) {
    case PROC_SAMPLING_ADD_NUTRI:
      if (elapsedTime >= dynamicNutriDuration) {
        setRelay(RELAY_NUTRI_AB_PIN, false); lcd.setCursor(0,1); lcd.print(F("2. Mixing...      "));
        setRelay(RELAY_MIXER_PIN, true); processStartTime = millis(); currentProcess = PROC_SAMPLING_MIXING;
      }
      break;
    case PROC_SAMPLING_MIXING:
      if (elapsedTime >= WAKTU_MIXING_MS) {
        setRelay(RELAY_MIXER_PIN, false); lcd.setCursor(0,1); lcd.print(F("3. Mengambil Sampel"));
        setRelay(RELAY_SAMPLING_PIN, true); processStartTime = millis(); currentProcess = PROC_SAMPLING_FILL_POT;
      }
      break;
    case PROC_SAMPLING_FILL_POT:
      if (elapsedTime >= WAKTU_ISI_WADAH_SENSOR_MS) {
        setRelay(RELAY_SAMPLING_PIN, false); lcd.setCursor(0,1); lcd.print(F("4. Membaca Sensor"));
        processStartTime = millis(); currentProcess = PROC_SAMPLING_READ;
      }
      break;
    case PROC_SAMPLING_READ:
      if (elapsedTime >= WAKTU_STABIL_SENSOR_MS) {
        updateSensors(); 
        // Nilai ini akan disimpan terus meskipun air nanti dibuang
        lastSamplingReady = evaluateSamplingReady(sensor_EC_uScm, sensor_pH);
        
        lastSamplingEC = sensor_EC_uScm; // Simpan nilai terakhir yg valid
        lastSamplingPH = sensor_pH;      // Simpan nilai terakhir yg valid

        lcd.clear();
        lastSamplingEC = sensor_EC_uScm; lastSamplingPH = sensor_pH;
        lastSamplingReady = evaluateSamplingReady(lastSamplingEC, lastSamplingPH);
        lcd.clear(); lcd.setCursor(0,0); lcd.print(F("EC:")); lcd.print((int)lastSamplingEC); lcd.print(F(" uS"));
        lcd.setCursor(0,1); lcd.print(F("pH:")); lcd.print(lastSamplingPH,2);
        
        if(lastSamplingReady) {
          lcd.setCursor(0,2); lcd.print(F("Stat: Siap Spray")); lcd.setCursor(0,3); lcd.print(F("5. Membuang Sampel"));
          setRelay(RELAY_BUANG_PIN, true); processStartTime = millis(); currentProcess = PROC_SAMPLING_DRAIN;
        } else {
          if (retryCount >= MAX_RETRY) {
             lcd.clear(); lcd.setCursor(0,0); lcd.print(F("GAGAL KOREKSI!"));
             lcd.setCursor(0,1); lcd.print(F("Cek Air/Sensor")); lcd.setCursor(0,2); lcd.print(F("Kembali ke IDLE"));
             digitalWrite(BUZZER_PIN, HIGH); delay(1000); digitalWrite(BUZZER_PIN, LOW);
             setRelay(RELAY_BUANG_PIN, true); processStartTime = millis(); currentProcess = PROC_SAMPLING_DRAIN; 
             lastSamplingReady = false; 
          } else {
             retryCount++; lcd.setCursor(0,2); lcd.print(F("Koreksi #")); lcd.print(retryCount);
             lcd.setCursor(0,3); lcd.print(F("A. Buang Lama"));
             setRelay(RELAY_BUANG_PIN, true); processStartTime = millis(); currentProcess = PROC_SAMPLING_DRAIN_BEFORE_CORRECT; 
          }
        }
      }
      break;

    case PROC_SAMPLING_DRAIN:
       if (elapsedTime >= WAKTU_BUANG_SAMPLING_MS) {
        setRelay(RELAY_BUANG_PIN, false);
        
        // JIKA MODE AUTO & SUKSES -> Lanjut Spray
        if (modeAuto && lastSamplingReady) { 
          lcd.clear(); lcd.setCursor(0,0); lcd.print(F("Nutrisi OK.")); 
          lcd.setCursor(0,1); lcd.print(F("Mulai Auto-Spray")); 
          sprayRoutine(); 
        } 
        // JIKA MANUAL ATAU GAGAL
        else {
          lcd.clear(); 
          
          // --- PERBAIKAN TAMPILAN STATUS ---
          if (lastSamplingReady) {
             // Kalau Sukses (Masuk Range)
             lcd.setCursor(0,0); lcd.print(F("Sampling SELESAI")); 
             lcd.setCursor(0,1); lcd.print(F("Status: TARGET OK")); 
          } else {
             // Kalau Gagal (Sudah 5x coba masih jelek)
             lcd.setCursor(0,0); lcd.print(F("Sampling GAGAL")); 
             lcd.setCursor(0,1); lcd.print(F("Status: TIMEOUT")); 
          }
          // ----------------------------------

          lcd.setCursor(0,2); lcd.print(F("EC:")); lcd.print((int)lastSamplingEC); lcd.print(F(" pH:")); lcd.print(lastSamplingPH,1);
          lcd.setCursor(0,3); lcd.print(F("Tekan ENTER/BACK")); 
          
          // Bunyi beep beda (Sukses=2x pendek, Gagal=1x panjang)
          if(lastSamplingReady) { beep(100); delay(100); beep(100); } 
          else { beep(1000); }
          
          currentProcess = PROC_IDLE; 
          waitingForAckAfterSampling = true;
        }
      }
      break;

    case PROC_SAMPLING_DRAIN_BEFORE_CORRECT:
      if (elapsedTime >= WAKTU_BUANG_SAMPLING_MS) {
        setRelay(RELAY_BUANG_PIN, false);

        // --- SAFETY & LOGIC UPGRADE ---
        if (!isSensorValid(lastSamplingPH, lastSamplingEC)) {
           lcd.clear(); 
           lcd.setCursor(0,0); lcd.print(F("!! SENSOR ERROR !!"));
           lcd.setCursor(0,1); lcd.print(F("pH/EC tdk Valid"));
           digitalWrite(BUZZER_PIN, HIGH); delay(2000); digitalWrite(BUZZER_PIN, LOW);
           currentProcess = PROC_IDLE; waitingForAckAfterSampling = true; 
           return; 
        } 

        // PRIORITAS: EC DULUAN
        float errorEC = setpoint_EC_uS - lastSamplingEC; // Hitung selisih/kekurangan

        if (errorEC > 50) { // Jika kurangnya lebih dari 50 uS (batas toleransi)
             lcd.clear(); lcd.setCursor(0,1); lcd.print(F("Koreksi: EC Kurang"));
             lcd.setCursor(0,2); lcd.print(F("Tambah Nutrisi..."));
             
             // --- LOGIKA ADAPTIF ---
             if (errorEC > 500) {
                // Jika selisih masih JAUH (> 500 uS), genjot pompa lebih lama
                dynamicNutriDuration = 6000; // 6 detik (~9.2 ml) -> Biar cepat naik
                lcd.setCursor(0,3); lcd.print(F("Mode: BOOST (6s)"));
             } 
             else if (errorEC > 200) {
                // Jika selisih SEDANG (> 200 uS)
                dynamicNutriDuration = 4000; // 4 detik (~6.1 ml)
                lcd.setCursor(0,3); lcd.print(F("Mode: NORMAL (4s)"));
             } 
             else {
                // Jika selisih DEKIT lagi (< 200 uS), pelan-pelan saja biar gak overshoot
                dynamicNutriDuration = 2000; // 2 detik (~3 ml)
                lcd.setCursor(0,3); lcd.print(F("Mode: FINE (2s)"));
             }
             // ----------------------

             setRelay(RELAY_NUTRI_AB_PIN, true); 
             processStartTime = millis(); 
             currentProcess = PROC_SAMPLING_ADD_NUTRI; 
             return; 
        }
      
        // SETELAH EC OK, BARU pH
        if (lastSamplingPH > (setpoint_pH + 0.2)) {
          lcd.clear(); lcd.setCursor(0,1); lcd.print(F("Koreksi: pH Basa"));
          lcd.setCursor(0,2); lcd.print(F("Dosing pH Down..."));
          setRelay(RELAY_PH_DOWN_PIN, true); 
          processStartTime = millis(); 
          currentProcess = PROC_CORRECT_PH_DOWN;
        } else if (lastSamplingPH < (setpoint_pH - 0.2)) {
          lcd.clear(); lcd.setCursor(0,1); lcd.print(F("Koreksi: pH Asam"));
          lcd.setCursor(0,2); lcd.print(F("Dosing pH Up..."));
          setRelay(RELAY_PH_UP_PIN, true); 
          processStartTime = millis(); 
          currentProcess = PROC_CORRECT_PH_UP;
        } else {
          // EC dan pH sudah OK
           if (modeAuto) {
             lcd.clear(); lcd.setCursor(0,0); lcd.print(F("Target Tercapai!"));
             lcd.setCursor(0,1); lcd.print(F("Nutrisi & pH OK."));
             delay(2000); sprayRoutine(); 
          } else {
             lcd.clear(); lcd.setCursor(0,0); lcd.print(F("Koreksi Selesai"));
             lcd.setCursor(0,1); lcd.print(F("EC & pH Target OK"));
             beep(500); currentProcess = PROC_IDLE; waitingForAckAfterSampling = true; 
          }
        }
      }
      break;

    case PROC_CORRECT_PH_UP:
      if (elapsedTime >= setpointDosingDur_ms) {
        setRelay(RELAY_PH_UP_PIN, false); lcd.setCursor(0,2); lcd.print(F("C. Mixing tandon..."));
        setRelay(RELAY_MIXER_PIN, true); processStartTime = millis(); currentProcess = PROC_WAIT_AFTER_CORRECTION;
      }
      break;
    case PROC_CORRECT_PH_DOWN:
      if (elapsedTime >= setpointDosingDur_ms) {
        setRelay(RELAY_PH_DOWN_PIN, false); lcd.setCursor(0,2); lcd.print(F("C. Mixing tandon..."));
        setRelay(RELAY_MIXER_PIN, true); processStartTime = millis(); currentProcess = PROC_WAIT_AFTER_CORRECTION;
      }
      break;
    case PROC_WAIT_AFTER_CORRECTION:
      if (elapsedTime >= WAKTU_MIXING_MS) {
        setRelay(RELAY_MIXER_PIN, false); lcd.clear();
        lcd.setCursor(0,1); lcd.print(F("Koreksi Selesai.")); lcd.setCursor(0,2); lcd.print(F("Mengambil sampel..."));
        setRelay(RELAY_SAMPLING_PIN, true); processStartTime = millis(); currentProcess = PROC_SAMPLING_FILL_POT;
      }
      break;
    case PROC_SPRAYING: 
      if (elapsedTime >= (unsigned long)setpointSprayDurationDetik * 1000) {
        setRelay(RELAY_SPRAY_PIN, false);
        if (modeAuto) { currentProcess = PROC_IDLE; showModeMenu(); } 
        else { lcd.setCursor(0,2); lcd.print(F("Selesai")); beep(200); currentProcess = PROC_IDLE; delay(1000); showMainMenu(); }
      }
      break;
  }
}

// ============ BUTTON CHECK ============
void checkButtonsAndHandle(){
  bool readingLeft = digitalRead(BTN_LEFT_PIN);
  bool readingRight = digitalRead(BTN_RIGHT_PIN);
  bool readingEnter = digitalRead(BTN_ENTER_PIN);
  bool readingBack = digitalRead(BTN_BACK_PIN);
  unsigned long now = millis();
  
  if (readingLeft == LOW && lastStateLeft == HIGH && (now - lastBtnTimeLeft) > btnDebounceDelay) {
    handleInput('L'); lastBtnTimeLeft = now;
  } lastStateLeft = readingLeft;
  
  if (readingRight == LOW && lastStateRight == HIGH && (now - lastBtnTimeRight) > btnDebounceDelay) {
    handleInput('R'); lastBtnTimeRight = now;
  } lastStateRight = readingRight;
  
  if (readingEnter == LOW && lastStateEnter == HIGH && (now - lastBtnTimeEnter) > btnDebounceDelay) {
    handleInput('E'); lastBtnTimeEnter = now;
  } lastStateEnter = readingEnter;
  
  if (readingBack == LOW && lastStateBack == HIGH && (now - lastBtnTimeBack) > btnDebounceDelay) {
    handleInput('B'); lastBtnTimeBack = now;
  } lastStateBack = readingBack;
}
//  if (currentProcess == PROC_IDLE && (millis() - lastInputTime < 3000)) return; //BUAT DELAY TOMBOL
void syncFirebase() {
  // 1. Cek Koneksi & Mode Aman
  if (WiFi.status() != WL_CONNECTED) return; 
  if (inCalibration || inSetpointAdjust) return;

  if (Firebase.ready() && signupOK) {
    
    // ============================================================
    // BAGIAN 1: BACA PERINTAH - Interval Cepat (200ms)
    // ============================================================
    static unsigned long lastReadCmd = 0;
    
    if (millis() - lastReadCmd > 200) { 
        lastReadCmd = millis();

        // --- A. PRIORITAS TINGGI (EMERGENCY STOP) ---
        if (Firebase.RTDB.getBool(&fbdo, "/control/cmd_emergency_stop")) {
            if (fbdo.boolData() == true) {
                Serial.println("\n>>> COMMAND: EMERGENCY STOP <<<");
                setRelay(RELAY_NUTRI_AB_PIN, false); setRelay(RELAY_MIXER_PIN, false);
                setRelay(RELAY_SAMPLING_PIN, false); setRelay(RELAY_BUANG_PIN, false);
                setRelay(RELAY_PH_UP_PIN, false);    setRelay(RELAY_PH_DOWN_PIN, false);
                setRelay(RELAY_SPRAY_PIN, false);    setRelay(RELAY_LAMPU_PIN, false);
                
                currentProcess = PROC_IDLE; 
                processStartTime = 0; 
                retryCount = 0; 
                waitingForAckAfterSampling = false;
                
                lcd.clear(); lcd.print(F("!!! REMOTE STOP !!!"));
                for(int i=0; i<3; i++) { beep(200); delay(100); }
                
                Firebase.RTDB.setBool(&fbdo, "/control/cmd_emergency_stop", false);
                delay(1500); showModeMenu(); 
                return;
            }
        }

        // --- B. MODE SWITCHING ---
        if (Firebase.RTDB.getBool(&fbdo, "/control/cmd_set_auto")) {
             if (fbdo.boolData()) {
                 if (!modeAuto) { 
                    modeAuto = true; 
                    EEPROM.begin(512); EEPROM.write(ADDR_MODE, 1); EEPROM.commit(); 
                    alreadySampledThisHour = true; 
                    lastModeChangeTime = millis(); 
                    lcd.clear(); lcd.print(F("Remote: SET AUTO")); 
                    delay(1000); showModeMenu(); 
                 }
                 Firebase.RTDB.setBool(&fbdo, "/control/cmd_set_auto", false);
             }
        }
        if (Firebase.RTDB.getBool(&fbdo, "/control/cmd_set_manual")) {
             if (fbdo.boolData()) {
                 if (modeAuto) { 
                    modeAuto = false; 
                    EEPROM.begin(512); EEPROM.write(ADDR_MODE, 0); EEPROM.commit(); 
                    lastModeChangeTime = millis(); 
                    lcd.clear(); lcd.print(F("Remote: SET MANUAL")); 
                    delay(1000); showModeMenu(); 
                 }
                 Firebase.RTDB.setBool(&fbdo, "/control/cmd_set_manual", false);
             }
        }

        // --- C. PERINTAH PROSES (Hanya jika IDLE) ---
        if (currentProcess == PROC_IDLE) {
            
            // 1. Start Spray
            if (Firebase.RTDB.getBool(&fbdo, "/control/cmd_start_spray")) {
                if (fbdo.boolData()) {
                    sprayRoutine(); 
                    lastFirebaseSync = 0; 
                    Firebase.RTDB.setBool(&fbdo, "/control/cmd_start_spray", false);
                }
            }
            
            // 2. Start Sampling
            if (Firebase.RTDB.getBool(&fbdo, "/control/cmd_start_sampling")) {
                if (fbdo.boolData()) {
                    samplingRoutine(); 
                    lastFirebaseSync = 0;
                    Firebase.RTDB.setBool(&fbdo, "/control/cmd_start_sampling", false);
                }
            }

            // ==========================================================
            // D. TEST MANUAL (LOGIKA BARU: FOLLOW APP STATE)
            // ==========================================================
            
            // 1. Mixer
            if (Firebase.RTDB.getBool(&fbdo, "/manual/relay_mixer")) {
                bool targetState = fbdo.boolData();
                if (digitalRead(RELAY_MIXER_PIN) != (targetState ? HIGH : LOW)) {
                    setRelay(RELAY_MIXER_PIN, targetState);
                    Serial.print("Manual Mixer: "); Serial.println(targetState ? "ON" : "OFF");
                }
            }

            // 2. Nutrisi
            if (Firebase.RTDB.getBool(&fbdo, "/manual/relay_nutri")) {
                bool targetState = fbdo.boolData();
                if (digitalRead(RELAY_NUTRI_AB_PIN) != (targetState ? HIGH : LOW)) {
                    setRelay(RELAY_NUTRI_AB_PIN, targetState);
                    Serial.print("Manual Nutri: "); Serial.println(targetState ? "ON" : "OFF");
                }
            }

            // 3. Sampling Pump
            if (Firebase.RTDB.getBool(&fbdo, "/manual/relay_sampling")) {
                bool targetState = fbdo.boolData();
                if (digitalRead(RELAY_SAMPLING_PIN) != (targetState ? HIGH : LOW)) {
                    setRelay(RELAY_SAMPLING_PIN, targetState);
                    Serial.print("Manual Sampling: "); Serial.println(targetState ? "ON" : "OFF");
                }
            }

            // 4. Buang (Drain)
            if (Firebase.RTDB.getBool(&fbdo, "/manual/relay_buang")) {
                bool targetState = fbdo.boolData();
                if (digitalRead(RELAY_BUANG_PIN) != (targetState ? HIGH : LOW)) {
                    setRelay(RELAY_BUANG_PIN, targetState);
                    Serial.print("Manual Buang: "); Serial.println(targetState ? "ON" : "OFF");
                }
            }

            // 5. Spray
            if (Firebase.RTDB.getBool(&fbdo, "/manual/relay_spray")) {
                bool targetState = fbdo.boolData();
                if (digitalRead(RELAY_SPRAY_PIN) != (targetState ? HIGH : LOW)) {
                    setRelay(RELAY_SPRAY_PIN, targetState);
                    Serial.print("Manual Spray: "); Serial.println(targetState ? "ON" : "OFF");
                }
            }

            // 6. pH Up
            if (Firebase.RTDB.getBool(&fbdo, "/manual/relay_ph_up")) {
                bool targetState = fbdo.boolData();
                if (digitalRead(RELAY_PH_UP_PIN) != (targetState ? HIGH : LOW)) {
                    setRelay(RELAY_PH_UP_PIN, targetState);
                    Serial.print("Manual pH Up: "); Serial.println(targetState ? "ON" : "OFF");
                }
            }

            // 7. pH Down
            if (Firebase.RTDB.getBool(&fbdo, "/manual/relay_ph_down")) {
                bool targetState = fbdo.boolData();
                if (digitalRead(RELAY_PH_DOWN_PIN) != (targetState ? HIGH : LOW)) {
                    setRelay(RELAY_PH_DOWN_PIN, targetState);
                    Serial.print("Manual pH Down: "); Serial.println(targetState ? "ON" : "OFF");
                }
            }

            // 8. Lampu
            if (Firebase.RTDB.getBool(&fbdo, "/manual/relay_lampu")) {
                bool targetState = fbdo.boolData();
                if (digitalRead(RELAY_LAMPU_PIN) != (targetState ? HIGH : LOW)) {
                    setRelay(RELAY_LAMPU_PIN, targetState);
                    Serial.print("Manual Lampu: "); Serial.println(targetState ? "ON" : "OFF");
                }
            }
        }
    }

    // ============================================================
    // BAGIAN 2: KIRIM DATA (MONITORING) - Interval 2 Detik
    // ============================================================
    if (millis() - lastFirebaseSync > SYNC_INTERVAL) {
        lastFirebaseSync = millis();
        
        DateTime now = rtc.now(); 
        FirebaseJson jsonMonitor;
        jsonMonitor.set("device_id", "ALAT_01");
        jsonMonitor.set("ph", pH);
        jsonMonitor.set("ec", EC);
        jsonMonitor.set("temp_air", sensor_temp_dht);
        jsonMonitor.set("lux", lux);
        jsonMonitor.set("hum", humUdara);
        jsonMonitor.set("water_temp", sensor_temp);
        jsonMonitor.set("system_mode", modeAuto ? "AUTO" : "MANUAL");
        jsonMonitor.set("timestamp", (int)now.unixtime());

        // Cek Status Relay
        String relayStatus = "STANDBY";
        if (digitalRead(RELAY_NUTRI_AB_PIN)) relayStatus = "NUTRISI PUMP";
        else if (digitalRead(RELAY_PH_UP_PIN)) relayStatus = "PH UP PUMP";
        else if (digitalRead(RELAY_PH_DOWN_PIN)) relayStatus = "PH DOWN PUMP";
        else if (digitalRead(RELAY_SPRAY_PIN)) relayStatus = "SPRAY PUMP";
        else if (digitalRead(RELAY_BUANG_PIN)) relayStatus = "DRAIN PUMP";
        else if (digitalRead(RELAY_SAMPLING_PIN)) relayStatus = "SAMPLE PUMP";
        else if (digitalRead(RELAY_MIXER_PIN)) relayStatus = "MIXER ON";
        else if (digitalRead(RELAY_LAMPU_PIN)) relayStatus = "LAMPU ON"; 
        
        jsonMonitor.set("active_relay", relayStatus); 

        String processText = "SIAGA (IDLE)";
        if (currentProcess != PROC_IDLE) processText = "PROSES AKTIF..."; 
        if (waitingForAckAfterSampling) processText = "SELESAI (Cek Data)";
        jsonMonitor.set("status_text", processText);

        Firebase.RTDB.setJSON(&fbdo, "/monitoring", &jsonMonitor);
        
        if (currentProcess != PROC_IDLE) {
           Firebase.RTDB.pushJSON(&fbdo, "/history_log", &jsonMonitor);
        }
        
        // --- Cek Setpoint ---
        if (!inSetpointAdjust && !inCalibration) {
           if (Firebase.RTDB.getJSON(&fbdo, "/setpoints")) {
             FirebaseJson &spJson = fbdo.jsonObject(); 
             FirebaseJsonData spData; 
             bool needSave = false;

             spJson.get(spData, "target_ph"); 
             if(spData.success) { float val = spData.floatValue; if(abs(val - setpointPH) > 0.1) { setpointPH = val; setpoint_pH = val; needSave = true; } }
             
             spJson.get(spData, "target_ec"); 
             if(spData.success) { float val = spData.floatValue; if(abs(val - setpointEC) > 10.0) { setpointEC = val; setpoint_EC_uS = val; needSave = true; } }

             spJson.get(spData, "spray_duration"); 
             if(spData.success) { int val = spData.intValue; if(val != setpointSprayDurationDetik) { setpointSprayDurationDetik = val; needSave = true; } }

             spJson.get(spData, "spray_interval"); 
             if(spData.success) { int val = spData.intValue; if(val != setpointSprayIntervalMenit) { setpointSprayIntervalMenit = val; needSave = true; } }

             if (needSave) { saveAllToEEPROM(); beep(50); }
           }
        }
    }
  }
}

// ============ ADC CALIBRATION HELPER ============
void setupADC(){
  // Konfigurasi Pin ADC untuk Sensor pH dan EC
  analogSetPinAttenuation(EC_PIN, ADC_11db);
  analogSetPinAttenuation(PH_PIN, ADC_11db);
  analogSetWidth(12); // Resolusi 12-bit (0-4095)
  
  // Alokasi memori untuk karakteristik kalibrasi ADC ESP32
  adc_chars = (esp_adc_cal_characteristics_t*)calloc(1, sizeof(esp_adc_cal_characteristics_t));
  
  // Karakterisasi ADC (Agar pembacaan voltase akurat)
  esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12, 0, adc_chars);
}

// ============ SETUP (SAFE OFFLINE MODE) ============
void setup() {
  Serial.begin(115200); delay(100);
  
  EEPROM.begin(512); 
  Wire.begin(I2C_LCD_SDA, I2C_LCD_SCL); 
  Wire1.begin(I2C_SENS_SDA, I2C_SENS_SCL);      
  lcd.init(); lcd.backlight();
  
  lightSensor.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, 0x23, &Wire1); 
  if (!rtc.begin(&Wire1)) { lcd.clear(); lcd.print(F("RTC ERROR")); delay(3000); }
  setupADC(); 
  DS18B20.begin(); DS18B20.setWaitForConversion(false); DS18B20.requestTemperatures(); 
  dht.begin();
  
  pinMode(RELAY_NUTRI_AB_PIN, OUTPUT); pinMode(RELAY_MIXER_PIN, OUTPUT);
  pinMode(RELAY_SAMPLING_PIN, OUTPUT); pinMode(RELAY_BUANG_PIN, OUTPUT);
  pinMode(RELAY_PH_UP_PIN, OUTPUT);    pinMode(RELAY_PH_DOWN_PIN, OUTPUT);
  pinMode(RELAY_SPRAY_PIN, OUTPUT);    pinMode(RELAY_LAMPU_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  
  digitalWrite(RELAY_NUTRI_AB_PIN, LOW); digitalWrite(RELAY_MIXER_PIN, LOW);
  digitalWrite(RELAY_SAMPLING_PIN, LOW); digitalWrite(RELAY_BUANG_PIN, LOW);
  digitalWrite(RELAY_PH_UP_PIN, LOW);    digitalWrite(RELAY_PH_DOWN_PIN, LOW);
  digitalWrite(RELAY_SPRAY_PIN, LOW);    digitalWrite(RELAY_LAMPU_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);
  
  pinMode(BTN_LEFT_PIN, INPUT_PULLUP); pinMode(BTN_RIGHT_PIN, INPUT_PULLUP);
  pinMode(BTN_ENTER_PIN, INPUT_PULLUP); pinMode(BTN_BACK_PIN, INPUT_PULLUP);
  
  loadAllFromEEPROM();
  if (slope == 0.0 && intercept == 0.0) { slope = -5.6; intercept = 15.5; }
  
  cursorInAutoMode = modeAuto; showModeMenu(); 
  
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lcd.clear(); lcd.print(F("Connect WiFi..."));
  
  int wifi_try = 0;
  while (WiFi.status() != WL_CONNECTED && wifi_try < 20) { 
    delay(500); lcd.print(F(".")); wifi_try++;
  }

  if(WiFi.status() == WL_CONNECTED) { 
    lcd.print(F(" OK!")); 
    delay(1000); 

    config.api_key = API_KEY;
    config.database_url = DATABASE_URL;
    config.token_status_callback = tokenStatusCallback; 
    
    if (Firebase.signUp(&config, &auth, "", "")){ signupOK = true; }
    Firebase.begin(&config, &auth);
    Firebase.reconnectWiFi(true);
    
  } else { 
    lcd.print(F(" FAIL!")); 
    delay(1000);
    lcd.clear(); 
    lcd.setCursor(0,0); lcd.print(F("Running OFFLINE"));
    lcd.setCursor(0,1); lcd.print(F("Mode Tanpa IOT"));
    delay(2000); 
  }

  esp_task_wdt_init(WDT_TIMEOUT, true); 
  esp_task_wdt_add(NULL); 
  
  showModeMenu();
}

// ============ MAIN LOOP (SMART RECONNECT) ============
void loop() {
  esp_task_wdt_reset(); 
  handleBuzzer();

  if (!inCalibration && !inSetpointAdjust) {
      static unsigned long lastWifiCheck = 0;
      if (millis() - lastWifiCheck > 10000) { 
        lastWifiCheck = millis();
        
        if (WiFi.status() != WL_CONNECTED) {
           WiFi.reconnect(); 
        } 
        else {
           if (!signupOK) {
               config.api_key = API_KEY;
               config.database_url = DATABASE_URL;
               if (Firebase.signUp(&config, &auth, "", "")){ 
                   signupOK = true; 
                   Firebase.begin(&config, &auth);
                   Firebase.reconnectWiFi(true);
                   beep(100); delay(100); beep(100); 
               }
           }
        }
      }
  }

  bool btnBackState = digitalRead(BTN_BACK_PIN);
  if (btnBackState == LOW && backBtnReleased) {
    backBtnReleased = false; unsigned long now = millis();
    if (now - lastBackPressTime <= DOUBLE_CLICK_SPEED) { backClickCount++; } 
    else { backClickCount = 1; }
    lastBackPressTime = now;
  }
  if (btnBackState == HIGH) backBtnReleased = true;
  
  if (backClickCount >= 2) {
    backClickCount = 0; 
    if (currentProcess != PROC_IDLE) { handleEmergencyConfirm(); }
  }
  if (backClickCount > 0 && (millis() - lastBackPressTime > DOUBLE_CLICK_SPEED)) { backClickCount = 0; }

  if (currentProcess != PROC_IDLE) {
    handleProcesses(); lastInputTime = millis(); 
  } else {
    handleSerial(); checkButtonsAndHandle();
    
    if (currentLevel == MANUAL_TEST_MENU) {
       static unsigned long lastManualRefresh = 0;
       if (millis() - lastManualRefresh > 500) {
          lastManualRefresh = millis();
          showManualTestMenu(false); 
       }
    }

    if (waitingForAckAfterSampling) {
       lastInputTime = millis(); 
    } else {
       if (modeAuto && !inCalibration && !inSetpointAdjust && currentLevel != MANUAL_TEST_MENU) {
         DateTime now = rtc.now(); 
         
         if (setpointSprayIntervalMenit > 0 && (now.minute() % setpointSprayIntervalMenit == 0)) {
           if (!alreadySprayedThisInterval) {
             lcd.clear(); lcd.setCursor(0,0); lcd.print(F("Auto-Spray ON"));
             lcd.setCursor(0,1); lcd.print(F("Durasi: ")); lcd.print(setpointSprayDurationDetik); lcd.print(F(" dtk"));
             setRelay(RELAY_SPRAY_PIN, true);
             processStartTime = millis(); currentProcess = PROC_SPRAYING;
             alreadySprayedThisInterval = true; 
           }
         } else { alreadySprayedThisInterval = false; }
   
         int intervalJam = setpointSampleIntervalJam;
         if (millis() > 60000 && (millis() - lastModeChangeTime > 10000) && intervalJam > 0 && (now.hour() % intervalJam == 0) && now.minute() == 0) {
           if (!alreadySampledThisHour) {
             lcd.clear(); lcd.setCursor(0,0); lcd.print(F("Jadwal Cek Nutrisi"));
             lcd.setCursor(0,1); lcd.print(F("Mulai sampling..."));
             samplingRoutine(); alreadySampledThisHour = true;
           }
         } else { if (now.minute() != 0) alreadySampledThisHour = false; }
   
         int jamSekarang = now.hour();
         if (jamSekarang >= 6 && jamSekarang < 20) {
             if (sensor_lux < setpointLux) setRelay(RELAY_LAMPU_PIN, true); 
             else setRelay(RELAY_LAMPU_PIN, false); 
         } else { setRelay(RELAY_LAMPU_PIN, false); }
         
         if (sensor_temp > setpointMaxTempWater) {
             if (millis() % 2000 < 200) digitalWrite(BUZZER_PIN, HIGH);
             else digitalWrite(BUZZER_PIN, LOW);
         }
       }
   
       if (inCalibration) { 
         runCalibrationDisplay(); 
         autoLoopActive = false; 
       }
       else if (inSetpointAdjust) { autoLoopActive = false; }
       else {
           unsigned long batasWaktuIdle = 5000; 
           if (currentLevel != MODE_MENU && currentLevel != MAIN_MENU) { batasWaktuIdle = 60000; }

           if (millis() - lastInputTime > batasWaktuIdle) {
             if (!autoLoopActive) {
               lcd.clear(); autoLoopActive = true; loopPage = 0; lastLoopChange = millis();
               if (currentLevel != MODE_MENU && currentLevel != MAIN_MENU) {
                   if (modeAuto) { currentLevel = MAIN_MENU; mainIndex = 0; } 
                   else { currentLevel = MODE_MENU; }
               }
             }
             handleAutoLoop(); 
           } else { autoLoopActive = false; }
        }
   
        if (currentLevel == MAIN_MENU && mainIndex == 0 && !autoLoopActive) { 
          unsigned long now = millis();
          if ((now - lastDisplay >= DISPLAY_INTERVAL) && (now - lastInputTime > 2000)) { 
            lastDisplay = now; updateSensors(); showMonitoring();
          }
        }
    } 
  }

  // === FIREBASE SYNC (Hanya Sync kalau tidak sedang setting) ===
  if (!inCalibration && !inSetpointAdjust) {
      syncFirebase(); 
  }
  
  delay(10); 
}