/*

  Open Gamma Detector Sketch
  Only works on the Raspberry Pi Pico with arduino-pico!

  Triggers on newly detected pulses and measures their energy.

  2023, NuclearPhoenix. Open Gamma Project.
  https://github.com/OpenGammaProject/Open-Gamma-Detector

  ## NOTE:
  ## Only change the highlighted USER SETTINGS below
  ## except you know exactly what you are doing!
  ## Flash with default settings and
  ##   Flash Size: "2MB (Sketch: 1984KB, FS: 64KB)"

  TODO: (?) Adafruit TinyUSB lib: WebUSB support
  TODO: (?) Software Threshold
  
  TODO: Retry a coincidence detection feature
  TODO: Add cps line trend to geiger mode
  TODO: Add custom display font

*/

#define _TASK_SCHEDULING_OPTIONS
#define _TASK_TIMECRITICAL       // Enable monitoring scheduling overruns
#define _TASK_SLEEP_ON_IDLE_RUN  // Enable 1 ms SLEEP_IDLE powerdowns between tasks if no callback methods were invoked during the pass
#include <TaskScheduler.h>       // Periodically executes tasks

//#include <ADCInput.h>     // Special SiPM readout utilizing the ADC FIFO and Round Robin
#include "hardware/vreg.h"  // Used for vreg_set_voltage
#include "Helper.h"         // Misc helper functions

#include <SimpleShell_Enhanced.h>  // Serial Commands/Console
#include <ArduinoJson.h>           // Load and save the settings file
#include <LittleFS.h>              // Used for FS, stores the settings and debug files
#include <RunningMedian.h>         // Used to get running median and average with circular buffers

/*
    ===================
    BEGIN USER SETTINGS
    ===================
*/
// These are the default settings that can only be changed by reflashing the Pico
#define SCREEN_TYPE SCREEN_SSD1306  // Display type: Either SCREEN_SSD1306 or SCREEN_SH1106
#define SCREEN_WIDTH 128            // OLED display width, in pixels
#define SCREEN_HEIGHT 64            // OLED display height, in pixels
#define SCREEN_ADDRESS 0x3C         // See datasheet for Address; 0x3D for 128x64, 0x3C for 128x32
#define PH_RESET 1                  // Milliseconds after which the P&H circuit will be reset once
#define EVENT_BUFFER 50000          // Buffer this many events for Serial.print
#define TRNG_BITS 8                 // Number of bits for each random number, max 8
#define BASELINE_NUM 101            // Number of measurements taken to determine the DC baseline
#define CONFIG_FILE "/config.json"  // File to store the settings
#define DEBUG_FILE "/debug.json"    // File to store some misc debug info
#define DISPLAY_REFRESH 1000        // Milliseconds between display refreshs

struct Config {
  // These are the default settings that can also be changes via the serial commands
  // Do not touch the struct itself, but only the values of the variables!
  bool ser_output = true;          // Wheter data should be Serial.println'ed
  bool geiger_mode = false;        // Measure only cps, not energy
  bool print_spectrum = false;     // Print the finishes spectrum, not just chronological events
  size_t meas_avg = 2;             // Number of meas. averaged each event, higher=longer dead time
  bool enable_display = true;     // Enable I2C Display, see settings above
  bool enable_trng = false;        // Enable the True Random Number Generator
  bool subtract_baseline = false;  // Subtract the DC bias from each pulse
  bool cps_correction = true;      // Correct the cps for the DNL compensation
  bool enable_ticker = true;      // Enable the buzzer to be used as a ticker for pulses
  size_t tick_rate = 20;           // Buzzer ticks once every tick_rate pulses

  // Do NOT modify the following operator function
  bool operator==(const Config &other) const {
    return (tick_rate == other.tick_rate && enable_ticker == other.enable_ticker && cps_correction == other.cps_correction && ser_output == other.ser_output && geiger_mode == other.geiger_mode && print_spectrum == other.print_spectrum && meas_avg == other.meas_avg && enable_display == other.enable_display && enable_trng == other.enable_trng && subtract_baseline == other.subtract_baseline);
  }
};
/*
    =================
    END USER SETTINGS
    =================
*/

const String FWVERS = "4.0.2rd";  // Firmware Version Code

const uint8_t GND_PIN = A2;    // GND meas pin
const uint8_t VSYS_MEAS = A3;  // VSYS/3
const uint8_t VBUS_MEAS = 24;  // VBUS Sense Pin
const uint8_t PS_PIN = 23;     // SMPS power save pin

const uint8_t AIN_PIN = A1;     // Analog input pin
const uint8_t AMP_PIN = A0;     // Preamp (baseline) meas pin
const uint8_t INT_PIN = 16;     // Signal interrupt pin
const uint8_t RST_PIN = 22;     // Peak detector MOSFET reset pin
const uint8_t LED = 25;         // Built-in LED on GP25
const uint8_t BUZZER_PIN = 9;   // Buzzer PWM pin for the ticker
const uint8_t BUTTON_PIN = 14;  // Misc button pin

const uint16_t BUZZER_FREQ = 2700;  // Frequency used for the buzzer PWM (resonance freq of the buzzer)
const uint8_t BUZZER_TICK = 10;     // On-time of the buzzer for a single pulse in ms
const uint16_t EVT_RESET_C = 65535;  // Number of counts after which the OLED stats will be reset
const uint16_t OUT_REFRESH = 1000;  // Milliseconds between serial data outputs

const float VREF_VOLTAGE = 3.0;  // ADC reference voltage, default is 3.0 with reference
const uint8_t ADC_RES = 12;      // Use 12-bit ADC resolution

volatile uint32_t spectrum[uint16_t(pow(2, ADC_RES))];          // Holds the output histogram (spectrum)
volatile uint32_t display_spectrum[uint16_t(pow(2, ADC_RES))];  // Holds the display histogram (spectrum)

volatile uint16_t events[EVENT_BUFFER];  // Buffer array for single events
volatile uint32_t event_position = 0;    // Target index in events array
volatile unsigned long start_time = 0;   // Time in ms when the spectrum collection has started
volatile unsigned long last_time = 0;    // Last time the display has been refreshed
volatile uint32_t last_total = 0;        // Last total pulse count for display

volatile unsigned long trng_stamps[3];     // Timestamps for True Random Number Generator
volatile uint8_t random_num = 0b00000000;  // Generated random bits that form a byte together
volatile uint8_t bit_index = 0;            // Bit index for the generated number
volatile uint32_t trng_nums[1000];         // TRNG number output array
volatile uint16_t number_index = 0;        // Amount of saved numbers to the TRNG array
volatile uint32_t total_events = 0;        // Total number of all registered pulses

RunningMedian baseline(BASELINE_NUM);  // Array of a number of baseline (DC bias) measurements at the SiPM input
uint16_t current_baseline = 0;         // Median value of the input baseline voltage

volatile bool adc_lock = false;  // Locks the ADC if it's currently in use

// Stores 5 * DISPLAY_REFRESH worth of "current" cps to calculate an average cps value in a ring buffer config
RunningMedian counts(5);

// Stores the last
RunningMedian dead_time(100);

// Configuration struct with all user settings
Config conf;

//ADCInput sipm(AMP_PIN);

// Check for the right display type
#if (SCREEN_TYPE == SCREEN_SH1106)
#include <Adafruit_SH110X.h>
#define DISPLAY_WHITE SH110X_WHITE
Adafruit_SH1106G display = Adafruit_SH1106G(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
#elif (SCREEN_TYPE == SCREEN_SSD1306)
#include <Adafruit_SSD1306.h>
#define DISPLAY_WHITE SSD1306_WHITE
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
#endif

// Forward declaration of callbacks
void writeDebugFileTime();
void queryButton();
void updateDisplay();
void dataOutput();
void updateBaseline();
void resetPHCircuit();

// Tasks
Task writeDebugFileTimeTask(60 * 60 * 1000, TASK_FOREVER, &writeDebugFileTime);
Task queryButtonTask(100, TASK_FOREVER, &queryButton);
Task updateDisplayTask(DISPLAY_REFRESH, TASK_FOREVER, &updateDisplay);
Task dataOutputTask(OUT_REFRESH, TASK_FOREVER, &dataOutput);
Task updateBaselineTask(1, TASK_FOREVER, &updateBaseline);
Task resetPHCircuitTask(PH_RESET, TASK_FOREVER, &resetPHCircuit);

// Scheduler
Scheduler schedule;


void resetSampleHold(uint8_t time = 2) {  // Reset sample and hold circuit
  digitalWriteFast(RST_PIN, HIGH);
  delayMicroseconds(time);  // Discharge for (default) 2 µs -> ~99% discharge time for 1 kOhm and 470 pF
  digitalWriteFast(RST_PIN, LOW);
}


void queryButton() {
  if ((BOOTSEL) || (digitalRead(8) == LOW)) {
    // Switch between Geiger and Energy modes
    conf.geiger_mode = !conf.geiger_mode;
    event_position = 0;
    clearSpectrum();
    clearSpectrumDisplay();
    resetSampleHold();

    if (conf.geiger_mode) {
      println("Switched to geiger mode.");
    } else {
      println("Switched to energy measuring mode.");
    }

    saveSettings();  // Saved updated settings

    while ((BOOTSEL) || (digitalRead(8) == LOW)) {      // Wait for BOOTSEL to be released
      delay(10);
      rp2040.wdt_reset();  // Reset watchdog so that the device doesn't quit if pressed for too long
    }
  }

  if (digitalRead(BUTTON_PIN) == LOW) {
    // Toggle the onboard ticker
    conf.enable_ticker = !conf.enable_ticker;

    if (conf.enable_ticker) {
      println("Enabled ticker output.");
    } else {
      println("Disabled ticker output.");
    }

    saveSettings();  // Saved updated settings

    while (digitalRead(BUTTON_PIN) == LOW) {  // Wait for the button to be released
      delay(10);
      rp2040.wdt_reset();  // Reset watchdog so that the device doesn't quit if pressed for too long
    }
  }

  rp2040.wdt_reset();  // Reset watchdog, everything is fine
}


void updateDisplay() {
  // Update display every DISPLAY_REFRESH ms
  // MAYBE ONLY START TASK IF DISPLAY IS ENABLED?
  if (conf.enable_display) {
    if (conf.geiger_mode) {
      drawGeigerCounts();
    } else {
      drawSpectrum();
    }
  }
}


void dataOutput() {
  // MAYBE ONLY START TASK IF OUTPUT IS ENABLED?
  if (conf.ser_output) {
    if (Serial || Serial2) {
      if (conf.print_spectrum) {
        for (uint16_t index = 0; index < uint16_t(pow(2, ADC_RES)); index++) {
          cleanPrint(String(spectrum[index]) + ";");
          //spectrum[index] = 0; // Uncomment for differential histogram
        }
        cleanPrintln();
      } else if (event_position > 0 && event_position <= EVENT_BUFFER) {
        for (uint16_t index = 0; index < event_position; index++) {
          cleanPrint(String(events[index]) + ";");
        }
        cleanPrintln();
      }
    }

    event_position = 0;
  }

  // MAYBE SEPARATE TRNG AND SERIAL OUTPUT TASKS?
  if (conf.enable_trng) {
    if (Serial || Serial2) {
      for (size_t i = 0; i < number_index; i++) {
        cleanPrint(trng_nums[i], DEC);
        cleanPrintln(";");
      }
      number_index = 0;
    }
  }
}


void updateBaseline() {
  static uint8_t baseline_done = 0;

  // Compute the median DC baseline to subtract from each pulse
  if (conf.subtract_baseline) {
    if (!adc_lock) {
      adc_lock = true;  // Disable interrupt ADC measurements while resetting
      baseline.add(analogRead(AIN_PIN));
      adc_lock = false;

      baseline_done++;

      if (baseline_done >= BASELINE_NUM) {
        current_baseline = round(baseline.getMedian());  // Take the median value

        baseline_done = 0;
      }
    }
  }
}


void resetPHCircuit() {
  if (!adc_lock) {
    adc_lock = true;    // Disable interrupt ADC measurements while resetting
    resetSampleHold();  // Periodically reset the S&H/P&H circuit
    adc_lock = false;
  }
  // TODO: Check if adc was locked and decrease interval to compensate
}


void setSerialOutMode(String *args) {
  String command = *args;
  command.replace("set out", "");
  command.trim();

  if (command == "spectrum") {
    conf.ser_output = true;
    conf.print_spectrum = true;
    println("Set serial output mode to spectrum histogram.");
  } else if (command == "events") {
    conf.ser_output = true;
    conf.print_spectrum = false;
    println("Set serial output mode to events.");
  } else if (command == "off") {
    conf.ser_output = false;
    conf.print_spectrum = false;
    println("Disabled serial outputs.");
  } else {
    println("Invalid input '" + command + "'.", true);
    println("Must be 'spectrum', 'events' or 'off'.", true);
    return;
  }
  saveSettings();
}


void toggleDisplay(String *args) {
  String command = *args;
  command.replace("set display", "");
  command.trim();

  if (command == "on") {
    conf.enable_display = true;
    println("Enabled display output. You might need to reboot the device.");
  } else if (command == "off") {
    conf.enable_display = false;
    println("Disabled display output. You might need to reboot the device.");
  } else {
    println("Invalid input '" + command + "'.", true);
    println("Must be 'on' or 'off'.", true);
    return;
  }
  saveSettings();
}


void toggleTicker(String *args) {
  String command = *args;
  command.replace("set ticker", "");
  command.trim();

  if (command == "on") {
    conf.enable_ticker = true;
    println("Enabled ticker output.");
  } else if (command == "off") {
    conf.enable_ticker = false;
    println("Disabled ticker output.");
  } else {
    println("Invalid input '" + command + "'.", true);
    println("Must be 'on' or 'off'.", true);
    return;
  }
  saveSettings();
}


void setTickerRate(String *args) {
  String command = *args;
  command.replace("set tickrate", "");
  command.trim();

  const long number = command.toInt();

  if (number > 0) {
    conf.tick_rate = number;
    println("Set ticker rate to " + String(number) + ".");
    saveSettings();
  } else {
    println("Invalid input '" + command + "'.", true);
    println("Parameter must be a number > 0.", true);
  }
}


void setMode(String *args) {
  String command = *args;
  command.replace("set mode", "");
  command.trim();

  if (command == "geiger") {
    conf.geiger_mode = true;
    event_position = 0;
    println("Enabled geiger mode.");
  } else if (command == "energy") {
    conf.geiger_mode = false;
    event_position = 0;
    println("Enabled energy measuring mode.");
  } else {
    println("Invalid input '" + command + "'.", true);
    println("Must be 'geiger' or 'energy'.", true);
    return;
  }
  resetSampleHold();
  saveSettings();
}


void toggleTRNG(String *args) {
  String command = *args;
  command.replace("set trng", "");
  command.trim();

  if (command == "on") {
    conf.enable_trng = true;
    println("Enabled True Random Number Generator output.");
  } else if (command == "off") {
    conf.enable_trng = false;
    println("Disabled True Random Number Generator output.");
  } else {
    println("Invalid input '" + command + "'.", true);
    println("Must be 'on' or 'off'.", true);
    return;
  }
  saveSettings();
}


void toggleBaseline(String *args) {
  String command = *args;
  command.replace("set baseline", "");
  command.trim();

  if (command == "on") {
    conf.subtract_baseline = true;
    println("Enabled automatic DC bias subtraction.");
  } else if (command == "off") {
    conf.subtract_baseline = false;
    current_baseline = 0;  // Reset baseline back to zero
    println("Disabled automatic DC bias subtraction.");
  } else {
    println("Invalid input '" + command + "'.", true);
    println("Must be 'on' or 'off'.", true);
    return;
  }
  saveSettings();
}


void toggleCPSCorrection(String *args) {
  String command = *args;
  command.replace("set correction", "");
  command.trim();

  if (command == "on") {
    conf.cps_correction = true;
    println("Enabled CPS correction.");
  } else if (command == "off") {
    conf.cps_correction = false;
    println("Disabled CPS correction.");
  } else {
    println("Invalid input '" + command + "'.", true);
    println("Must be 'on' or 'off'.", true);
    return;
  }
  saveSettings();
}


void setMeasAveraging(String *args) {
  String command = *args;
  command.replace("set averaging", "");
  command.trim();
  const long number = command.toInt();

  if (number > 0) {
    conf.meas_avg = number;
    println("Set measurement averaging to " + String(number) + ".");
    saveSettings();
  } else {
    println("Invalid input '" + command + "'.", true);
    println("Parameter must be a number > 0.", true);
  }
}


void deviceInfo([[maybe_unused]] String *args) {
  File debugFile = LittleFS.open(DEBUG_FILE, "r");

  DynamicJsonDocument doc(512);
  DeserializationError error = deserializeJson(doc, debugFile);

  uint32_t power_cycle, power_on;

  if (!debugFile || error) {
    power_cycle = 0;
    power_on = 0;
  } else {
    power_cycle = doc["power_cycle_count"];
    power_on = doc["power_on_hours"];
  }

  float avg_dt = dead_time.getMedianAverage(50);

  debugFile.close();

  println("=========================");
  println("-- Open Gamma Detector --");
  println("By NuclearPhoenix, Open Gamma Project");
  println("2023. https://github.com/OpenGammaProject");
  println("Firmware Version: " + FWVERS);
  println("=========================");
  println("Runtime: " + String(millis() / 1000.0) + " s");
  print("Average Dead Time: ");

  if (total_events == 0) {
    cleanPrint("no impulses");
  } else {
    cleanPrint(String(round(avg_dt), 0));
  }

  cleanPrintln(" µs");

  const float deadtime_frac = avg_dt * total_events / 1000.0 / float(millis()) * 100.0;

  println("Total Dead Time: " + String(deadtime_frac) + " %");
  println("Total Number Of Impulses: " + String(total_events));
  println("CPU Frequency: " + String(rp2040.f_cpu() / 1e6) + " MHz");
  println("Used Heap Memory: " + String(rp2040.getUsedHeap() / 1000.0) + " kB");
  println("Free Heap Memory: " + String(rp2040.getFreeHeap() / 1000.0) + " kB");
  println("Total Heap Size: " + String(rp2040.getTotalHeap() / 1000.0) + " kB");
  println("Temperature: " + String(round(readTemp() * 10.0) / 10.0, 1) + " °C");
  println("USB Connection: " + String(digitalRead(VBUS_MEAS)));

  const float v = 3.0 * analogRead(VSYS_MEAS) * VREF_VOLTAGE / (pow(2, ADC_RES) - 1);

  println("Supply Voltage: " + String(round(v * 10.0) / 10.0, 1) + " V");

  print("Power Cycle Count: ");
  if (power_cycle == 0) {
    cleanPrintln("n/a");
  } else {
    cleanPrintln(power_cycle);
  }

  print("Power-on hours: ");
  if (power_on == 0) {
    cleanPrintln("n/a");
  } else {
    cleanPrintln(power_on);
  }
}


void fsInfo([[maybe_unused]] String *args) {
  FSInfo fsinfo;
  LittleFS.info(fsinfo);
  println("Total Size: " + String(fsinfo.totalBytes / 1000.0) + " kB");
  print("Used Size: " + String(fsinfo.usedBytes / 1000.0) + " kB");
  cleanPrintln(" / " + String(float(fsinfo.usedBytes) / fsinfo.totalBytes * 100) + " %");
  println("Block Size: " + String(fsinfo.blockSize / 1000.0) + " kB");
  println("Page Size: " + String(fsinfo.pageSize) + " B");
  println("Max Open Files: " + String(fsinfo.maxOpenFiles));
  println("Max Path Length: " + String(fsinfo.maxPathLength));
}


void getSpectrumData([[maybe_unused]] String *args) {
  cleanPrintln();
  println("Pulse height histogram:");
  for (size_t i = 0; i < pow(2, ADC_RES); i++) {
    cleanPrintln(spectrum[i]);
  }
  cleanPrintln();
  println("Hint: To import this data into Gamma MCA, you have to replace all the ';' with a new line '\n'.");
}


void clearSpectrumData([[maybe_unused]] String *args) {
  println("Resetting spectrum...");
  clearSpectrum();
  //clearSpectrumDisplay();
  println("Successfully reset spectrum!");
}


void readSettings([[maybe_unused]] String *args) {
  File saveFile = LittleFS.open(CONFIG_FILE, "r");

  if (!saveFile) {
    println("Could not open save file!", true);
    return;
  }

  DynamicJsonDocument doc(1024);
  DeserializationError error = deserializeJson(doc, saveFile);

  saveFile.close();

  if (error) {
    print("Could not load config from json file: ", true);
    cleanPrintln(error.f_str());
    return;
  }

  serializeJsonPretty(doc, Serial);

  cleanPrintln();
  println("Read settings file successfully.");
}


void resetSettings([[maybe_unused]] String *args) {
  Config defaultConf;  // New Config object with all default parameters
  conf = defaultConf;
  println("Applied default settings.");
  println("You might need to reboot for all changes to take effect.");

  saveSettings();
}


void rebootNow([[maybe_unused]] String *args) {
  println("You might need to reconnect after reboot.");
  println("Rebooting now...");
  delay(1000);
  rp2040.reboot();
}


void clearSpectrum() {
  for (size_t i = 0; i < pow(2, ADC_RES); i++) {
    spectrum[i] = 0;
  }
}


void clearSpectrumDisplay() {
  for (size_t i = 0; i < pow(2, ADC_RES); i++) {
    display_spectrum[i] = 0;
  }
  start_time = millis();  // Spectrum pulse collection has started
  last_time = millis();
  last_total = 0;  // Remove old values
}


void readDebugFile() {
  File debugFile = LittleFS.open(DEBUG_FILE, "r");

  if (!debugFile) {
    println("Could not open debug file!", true);
    return;
  }

  DynamicJsonDocument doc(512);
  DeserializationError error = deserializeJson(doc, debugFile);

  debugFile.close();

  if (error) {
    print("Could not load debug info from json file: ", true);
    cleanPrintln(error.f_str());
    return;
  }

  serializeJsonPretty(doc, Serial);

  cleanPrintln();
  println("Read debug file successfully.");
}


void writeDebugFileTime() {
  // ALMOST THE SAME AS THE BOOT DEBUG FILE WRITE!
  File debugFile = LittleFS.open(DEBUG_FILE, "r");  // Open read and write

  DynamicJsonDocument doc(512);
  DeserializationError error = deserializeJson(doc, debugFile);

  if (!debugFile || error) {
    //println("Could not open debug file!", true);
    print("Could not load debug info from json file: ", true);
    cleanPrintln(error.f_str());

    doc["power_cycle_count"] = 0;
    doc["power_on_hours"] = 0;
  }

  debugFile.close();

  uint32_t temp = 0;
  if (doc.containsKey("power_on_hours")) {
    temp = doc["power_on_hours"];
  }
  doc["power_on_hours"] = ++temp;

  debugFile = LittleFS.open(DEBUG_FILE, "w");  // Open read and write
  serializeJson(doc, debugFile);
  debugFile.close();
}


void writeDebugFileBoot() {
  // ALMOST THE SAME AS THE TIME DEBUG FILE WRITE!
  File debugFile = LittleFS.open(DEBUG_FILE, "r");  // Open read and write

  DynamicJsonDocument doc(512);
  DeserializationError error = deserializeJson(doc, debugFile);

  if (!debugFile || error) {
    //println("Could not open debug file!", true);
    print("Could not load debug info from json file: ", true);
    cleanPrintln(error.f_str());

    doc["power_cycle_count"] = 0;
    doc["power_on_hours"] = 0;
  }

  debugFile.close();

  uint32_t temp = 0;
  if (doc.containsKey("power_cycle_count")) {
    temp = doc["power_cycle_count"];
  }
  doc["power_cycle_count"] = ++temp;

  debugFile = LittleFS.open(DEBUG_FILE, "w");  // Open read and write
  serializeJson(doc, debugFile);
  debugFile.close();
}


Config loadSettings(bool msg = true) {
  Config new_conf;
  File saveFile = LittleFS.open(CONFIG_FILE, "r");

  if (!saveFile) {
    println("Could not open save file! Creating a fresh file...", true);

    writeSettingsFile();  // Force creation of a new file

    return new_conf;
  }

  DynamicJsonDocument doc(1024);
  DeserializationError error = deserializeJson(doc, saveFile);

  saveFile.close();

  if (error) {
    print("Could not load config from json file: ", true);
    cleanPrintln(error.f_str());
    return new_conf;
  }

  if (doc.containsKey("ser_output")) {
    new_conf.ser_output = doc["ser_output"];
  }
  if (doc.containsKey("geiger_mode")) {
    new_conf.geiger_mode = doc["geiger_mode"];
  }
  if (doc.containsKey("print_spectrum")) {
    new_conf.print_spectrum = doc["print_spectrum"];
  }
  if (doc.containsKey("meas_avg")) {
    new_conf.meas_avg = doc["meas_avg"];
  }
  if (doc.containsKey("enable_display")) {
    new_conf.enable_display = doc["enable_display"];
  }
  if (doc.containsKey("enable_trng")) {
    new_conf.enable_trng = doc["enable_trng"];
  }
  if (doc.containsKey("subtract_baseline")) {
    new_conf.subtract_baseline = doc["subtract_baseline"];
  }
  if (doc.containsKey("cps_correction")) {
    new_conf.cps_correction = doc["cps_correction"];
  }
  if (doc.containsKey("enable_ticker")) {
    new_conf.enable_ticker = doc["enable_ticker"];
  }
  if (doc.containsKey("tick_rate")) {
    new_conf.tick_rate = doc["tick_rate"];
  }

  if (msg) {
    println("Successfuly loaded settings from flash.");
  }
  return new_conf;
}


bool writeSettingsFile() {
  File saveFile = LittleFS.open(CONFIG_FILE, "w");

  if (!saveFile) {
    println("Could not open save file!", true);
    return false;
  }

  DynamicJsonDocument doc(1024);

  doc["ser_output"] = conf.ser_output;
  doc["geiger_mode"] = conf.geiger_mode;
  doc["print_spectrum"] = conf.print_spectrum;
  doc["meas_avg"] = conf.meas_avg;
  doc["enable_display"] = conf.enable_display;
  doc["enable_trng"] = conf.enable_trng;
  doc["subtract_baseline"] = conf.subtract_baseline;
  doc["cps_correction"] = conf.cps_correction;
  doc["enable_ticker"] = conf.enable_ticker;
  doc["tick_rate"] = conf.tick_rate;

  serializeJson(doc, saveFile);

  saveFile.close();

  return true;
}


bool saveSettings() {
  Config read_conf = loadSettings(false);

  if (read_conf == conf) {
    //println("Settings did not change... not writing to flash.");
    return false;
  }

  //println("Successfuly written config to flash.");
  return writeSettingsFile();
}


void serialEvent() {
  Shell.handleEvent();  // Handle the serial input for the USB Serial
}


void serialEvent2() {
  Shell.handleEvent();  // Handle the serial input for the Hardware Serial
}


float readTemp() {
  adc_lock = true;                        // Flag this, so that nothing else uses the ADC in the mean time
  delayMicroseconds(conf.meas_avg * 20);  // Wait for an already-executing interrupt
  const float temp = analogReadTemp(VREF_VOLTAGE);
  adc_lock = false;
  return temp;
}


void drawSpectrum() {
  const uint16_t BINSIZE = floor(pow(2, ADC_RES-1) / SCREEN_WIDTH);
  uint32_t eventBins[SCREEN_WIDTH];
  uint16_t offset = 0;
  uint32_t max_num = 0;
  uint32_t total = 0;

  for (uint16_t i = 0; i < SCREEN_WIDTH; i++) {
    uint32_t totalValue = 0;

    for (uint16_t j = offset; j < offset + BINSIZE; j++) {
      totalValue += display_spectrum[j];
    }

    offset += BINSIZE;
    eventBins[i] = totalValue;

    if (totalValue > max_num) {
      max_num = totalValue;
    }

    total += totalValue;
  }

  unsigned long now_time = millis();

  float scale_factor = 0.0;

  if (max_num > 0) {  // No events accumulated, catch divide by zero
    scale_factor = float(SCREEN_HEIGHT - 11) / float(max_num);
  }

  uint32_t new_total = total - last_total;
  last_total = total;

  if (now_time < last_time) {  // Catch Millis() Rollover
    last_time = now_time;
    return;
  }

  unsigned long time_delta = now_time - last_time;
  last_time = now_time;

  if (time_delta == 0) {  // Catch divide by zero
    time_delta = 1000;
  }

  display.clearDisplay();
  display.setCursor(0, 0);

  counts.add(new_total * 1000.0 / time_delta);

  float avg_cps = counts.getAverage();
  float avg_dt = dead_time.getAverage();
  float avg_cps_corrected = avg_cps;
  if (avg_dt > 0.) {
    avg_cps_corrected = avg_cps / (1.0 - avg_cps * avg_dt / 1.0e6);
  }

  display.print(avg_cps_corrected, 1);
  display.print(" cps");

  static int16_t temp = round(readTemp());

  if (!adc_lock) {  // Only update if ADC is free atm
    temp = round(readTemp());
  }

  if (temp < 0) {
    display.setCursor(SCREEN_WIDTH - 36, 0);
  } else {
    display.setCursor(SCREEN_WIDTH - 30, 0);
  }
  display.print(temp);
  display.print(" ");
  display.print((char)247);
  display.println("C");

  const unsigned long seconds_running = round((millis() - start_time) / 1000.0);
  const uint8_t char_offset = floor(log10(seconds_running));

  display.setCursor(SCREEN_WIDTH - 18 - char_offset * 6, 8);
  display.print(seconds_running);
  display.println(" s");

  for (uint16_t i = 0; i < SCREEN_WIDTH; i++) {
    uint32_t val = round(eventBins[i] * scale_factor);
    display.drawFastVLine(i, SCREEN_HEIGHT - val - 1, val, DISPLAY_WHITE);
  }
  display.drawFastHLine(0, SCREEN_HEIGHT - 1, SCREEN_WIDTH, DISPLAY_WHITE);

  display.display();

  if (total > EVT_RESET_C) {
    clearSpectrumDisplay();
  }
}


void drawGeigerCounts() {
  uint32_t total = 0;

  for (uint16_t i = 0; i < pow(2, ADC_RES); i++) {
    total += display_spectrum[i];
  }

  unsigned long now_time = millis();

  uint32_t new_total = total - last_total;
  last_total = total;

  if (now_time < last_time) {  // Catch Millis() Rollover
    last_time = now_time;
    return;
  }

  unsigned long time_delta = now_time - last_time;
  last_time = now_time;

  if (time_delta == 0) {  // Catch divide by zero
    time_delta = 1000;
  }

  counts.add(new_total * 1000.0 / time_delta);

  float avg_cps = counts.getAverage();
  float avg_dt = dead_time.getAverage();  //+ 5.0;
  float avg_cps_corrected = avg_cps;
  if (avg_dt > 0.) {
    avg_cps_corrected = avg_cps / (1.0 - avg_cps * avg_dt / 1.0e6);
  }

  static float max_cps = -1;
  static float min_cps = -1;

  if (max_cps == -1 || avg_cps_corrected > max_cps) {
    max_cps = avg_cps_corrected;
  }
  if (min_cps <= 0 || avg_cps_corrected < min_cps) {
    min_cps = avg_cps_corrected;
  }

  display.clearDisplay();
  display.setCursor(0, 0);

  display.print("Min: ");
  display.println(min_cps, 1);

  display.print("Max: ");
  display.println(max_cps, 1);

  static int16_t temp = round(readTemp());

  if (!adc_lock) {  // Only update if ADC is free atm
    temp = round(readTemp());
  }

  if (temp < 0) {
    display.setCursor(SCREEN_WIDTH - 36, 0);
  } else {
    display.setCursor(SCREEN_WIDTH - 30, 0);
  }
  display.print(temp);
  display.print(" ");
  display.print((char)247);
  display.println("C");

  display.setCursor(0, 0);
  display.setTextSize(2);

  display.drawFastHLine(0, 18, SCREEN_WIDTH, DISPLAY_WHITE);

  display.setCursor(0, 26);

  if (avg_cps_corrected > 1000) {
    display.print(avg_cps_corrected / 1000.0, 2);
    display.print("k");
  } else {
    display.print(avg_cps_corrected, 1);
  }

  display.setTextSize(1);
  display.println(" cps");

  display.display();

  if (total > EVT_RESET_C) {
    clearSpectrumDisplay();
  }
}


void eventInt() {
  // Disable interrupt generation for this pin ASAP.
  // Directly uses Core0 IRQ Ctrl (core1 does not set the interrupt).
  // Thanks a lot to all the replies at
  // https://github.com/earlephilhower/arduino-pico/discussions/1397!
  static io_rw_32 *addr = &(iobank0_hw->proc0_irq_ctrl.inte[INT_PIN / 8]);
  static uint32_t mask1 = 0b1111 << (INT_PIN % 8) * 4u;
  hw_clear_bits(addr, mask1);

  const unsigned long start = micros();

  digitalWriteFast(LED, HIGH);  // Enable activity LED

  //const unsigned long start_millis = millis();
  //static unsigned long last_tick = start_millis;  // Last buzzer tick in ms, not needed with tone()
  static uint8_t count = 0;

  // Check if ticker is enabled, currently not "ticking" and also catch the millis() overflow
  if (conf.enable_ticker /* && (start_millis - last_tick > BUZZER_TICK || start_millis < last_tick)*/) {
    if (count >= conf.tick_rate - 1) {             // Only click at every 10th count
      tone(BUZZER_PIN, BUZZER_FREQ, BUZZER_TICK);  // Worse at higher cps
      //last_tick = start_millis;
      count = 0;
    } else {
      count++;
    }
  }

  uint16_t mean = 0;

  if (!conf.geiger_mode && !adc_lock) {
    uint32_t sum = 0;
    uint8_t num = 0;

    for (size_t i = 0; i < conf.meas_avg; i++) {
      const uint16_t m = analogRead(AIN_PIN);
      // Pico-ADC DNL issues, see https://pico-adc.markomo.me/INL-DNL/#dnl
      // Discard channels 512, 1536, 2560, and 3584. For now.
      // See RP2040 datasheet Appendix B: Errata
      if (m == 511 || m == 1535 || m == 2559 || m == 3583) {
        continue;  // Discard single measurement
        //break; // Completely disregard this measurement entirely
      }
      sum += m;
      num++;
    }

    resetSampleHold();

    float avg = 0.0;  // Use median instead of average?

    if (num > 0) {
      avg = float(sum) / float(num);
    }

    if (current_baseline <= avg) {  // Catch negative numbers
      // Subtract DC bias from pulse avg and then convert float --> uint16_t ADC channel
      mean = round(avg - current_baseline);
    }
  }

  if ((conf.ser_output || conf.enable_display) && (conf.cps_correction || mean != 0 || conf.geiger_mode)) {
    events[event_position] = mean;
    spectrum[mean] += 1;
    display_spectrum[mean] += 1;
    if (event_position >= EVENT_BUFFER - 1) {  // Increment if memory available, else overwrite array
      event_position = 0;
    } else {
      event_position++;
    }
  }

  if (conf.enable_trng) {
    static uint8_t trng_index = 0;  // Timestamp index for True Random Number Generator

    // Calculations for the TRNG
    trng_stamps[trng_index] = micros();

    if (trng_index < 2) {
      trng_index++;
    } else {
      // Catch micros() overflow
      if (trng_stamps[1] > trng_stamps[0] && trng_stamps[2] > trng_stamps[1]) {
        const uint32_t delta0 = trng_stamps[1] - trng_stamps[0];
        const uint32_t delta1 = trng_stamps[2] - trng_stamps[1];

        if (delta0 < delta1) {
          bitWrite(random_num, bit_index, 0);
        } else {
          bitWrite(random_num, bit_index, 1);
        }

        if (bit_index < TRNG_BITS - 1) {
          bit_index++;
        } else {
          trng_nums[number_index] = random_num;

          if (number_index < 999) {
            number_index++;
          } else {
            number_index = 0;  // Catch overflow
          }

          random_num = 0b00000000;  // Clear number
          bit_index = 0;
        }
      }

      trng_index = 0;
    }
  }

  digitalWriteFast(LED, LOW);  // Disable activity LED

  const unsigned long end = micros();

  if (end >= start) {  // Catch micros() overflow
    // Compute Detector Dead Time
    total_events++;
    dead_time.add(end - start);
  }

  // Re-enable interrupts
  static uint32_t mask2 = 0b0100 << (INT_PIN % 8) * 4u;
  hw_set_bits(addr, mask2);

  // Clear all interrupts on the executing core
  irq_clear(15);  // IRQ 15 = SIO_IRQ_PROC0
}

/*
    SETUP FUNCTIONS
*/
void setup() {
  pinMode(AMP_PIN, INPUT);
  pinMode(INT_PIN, INPUT);
  pinMode(AIN_PIN, INPUT);
  pinMode(RST_PIN, OUTPUT_12MA);
  pinMode(LED, OUTPUT);

  //gpio_set_slew_rate(RST_PIN, GPIO_SLEW_RATE_SLOW);  // Slow slew rate to reduce EMI
  gpio_set_slew_rate(LED, GPIO_SLEW_RATE_SLOW);  // Slow slew rate to reduce EMI

  analogReadResolution(ADC_RES);

  resetSampleHold(5);  // Reset before enabling the interrupts to avoid jamming

  //sipm.setBuffers(4, 64);
  //sipm.onReceive(eventInt);
  //sipm.begin(200000);

  attachInterrupt(digitalPinToInterrupt(INT_PIN), eventInt, FALLING);

  start_time = millis();  // Spectrum pulse collection has started
}


void setup1() {
  rp2040.wdt_begin(5000);  // Enable hardware watchdog to check every 5s

  // Undervolt a bit to save power, pretty conservative value could be even lower probably
  vreg_set_voltage(VREG_VOLTAGE_1_00);

  // Disable "Power-Saving" power supply option.
  // -> does not actually significantly save power, but output is much less noisy in HIGH!
  // -> Also with PS_PIN LOW I have experiences high-pitched (~ 15 kHz range) coil whine!
  pinMode(PS_PIN, OUTPUT_4MA);
  gpio_set_slew_rate(PS_PIN, GPIO_SLEW_RATE_SLOW);  // Slow slew rate to reduce EMI
  // Not really sure what results in better spectra...
  // It doesn't change a whole lot, so might as well keep the energy saving mode?
  digitalWrite(PS_PIN, LOW);
  //digitalWrite(PS_PIN, HIGH);

  pinMode(GND_PIN, INPUT);
  pinMode(VSYS_MEAS, INPUT);
  pinMode(VBUS_MEAS, INPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  Shell.registerCommand(new ShellCommand(getSpectrumData, "read spectrum", "Read the spectrum histogram collected since the last reset."));
  Shell.registerCommand(new ShellCommand(readSettings, "read settings", "Read the current settings (file)."));
  Shell.registerCommand(new ShellCommand(deviceInfo, "read info", "Read misc info about the firmware and state of the device."));
  Shell.registerCommand(new ShellCommand(fsInfo, "read fs", "Read misc info about the used filesystem."));

  Shell.registerCommand(new ShellCommand(toggleBaseline, "set baseline", "<toggle> Automatically subtract the DC bias (baseline) from each signal."));
  Shell.registerCommand(new ShellCommand(toggleTRNG, "set trng", "<toggle> Either 'on' or 'off' to toggle the true random number generator output."));
  Shell.registerCommand(new ShellCommand(toggleDisplay, "set display", "<toggle> Either 'on' or 'off' to enable or force disable OLED support."));
  Shell.registerCommand(new ShellCommand(toggleCPSCorrection, "set correction", "<toggle> Either 'on' or 'off' to toggle the CPS correction for the 4 faulty ADC channels."));

  Shell.registerCommand(new ShellCommand(setMode, "set mode", "<mode> Either 'geiger' or 'energy' to disable or enable energy measurements. Geiger mode only counts pulses, but is ~3x faster."));
  Shell.registerCommand(new ShellCommand(setSerialOutMode, "set out", "<mode> Either 'events', 'spectrum' or 'off'. 'events' prints events as they arrive, 'spectrum' prints the accumulated histogram."));
  Shell.registerCommand(new ShellCommand(setMeasAveraging, "set averaging", "<number> Number of ADC averages for each energy measurement. Takes ints, minimum is 1."));
  Shell.registerCommand(new ShellCommand(setTickerRate, "set tickrate", "<number> Rate at which the buzzer ticks, ticks once every <number> of pulses. Takes ints, minimum is 1."));
  Shell.registerCommand(new ShellCommand(toggleTicker, "set ticker", "<toggle> Either 'on' or 'off' to enable or disable the onboard ticker."));

  Shell.registerCommand(new ShellCommand(clearSpectrumData, "reset spectrum", "Reset the on-board spectrum histogram."));
  Shell.registerCommand(new ShellCommand(resetSettings, "reset settings", "Reset all the settings/revert them back to default values."));
  Shell.registerCommand(new ShellCommand(rebootNow, "reboot", "Reboot the device."));

  // Starts FileSystem, autoformats if no FS is detected
  LittleFS.begin();
  conf = loadSettings();  // Read all the detector settings from flash

  saveSettings();        // Create settings file if none is present
  writeDebugFileBoot();  // Update power cycle count

  // Set the correct SPI pins
  SPI.setRX(4);
  SPI.setTX(3);
  SPI.setSCK(2);
  SPI.setCS(5);
  // Set the correct I2C pins
  Wire.setSDA(0);
  Wire.setSCL(1);
  // Set the correct UART pins
  Serial2.setRX(9);
  Serial2.setTX(8);

  // Set up buzzer
  pinMode(BUZZER_PIN, OUTPUT_12MA);
  gpio_set_slew_rate(BUZZER_PIN, GPIO_SLEW_RATE_SLOW);  // Slow slew rate to reduce EMI
  digitalWrite(BUZZER_PIN, LOW);

  Shell.begin(2000000);
  Serial2.begin(2000000);

  println("Welcome to the Open Gamma Detector!");
  println("Firmware Version " + FWVERS);

  if (conf.enable_display) {
    bool begin = false;

#if (SCREEN_TYPE == SCREEN_SSD1306)
    begin = display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS);
#elif (SCREEN_TYPE == SCREEN_SH1106)
    begin = display.begin(SCREEN_ADDRESS, true);
#endif

    if (!begin) {
      println("Failed communication with the display. Maybe the I2C address is incorrect?", true);
      conf.enable_display = false;
    } else {
      display.setRotation(0);
      display.setTextSize(2);  // Draw 2X-scale text
      display.setTextColor(DISPLAY_WHITE);

      display.clearDisplay();

      display.println("Open Gamma Detector");
      display.println();
      display.setTextSize(1);
      display.print("FW ");
      display.println(FWVERS);
      display.drawBitmap(128 - 34, 64 - 34, opengamma_pcb, 32, 32, DISPLAY_WHITE);

      display.display();
      //delay(2000);
    }
  }

  // Set up task scheduler and enable tasks
  updateDisplayTask.setSchedulingOption(TASK_INTERVAL);  // TASK_SCHEDULE, TASK_SCHEDULE_NC, TASK_INTERVAL
  dataOutputTask.setSchedulingOption(TASK_INTERVAL);
  //resetPHCircuitTask.setSchedulingOption(TASK_INTERVAL);
  //updateBaselineTask.setSchedulingOption(TASK_INTERVAL);

  schedule.init();
  schedule.allowSleep(true);

  schedule.addTask(writeDebugFileTimeTask);
  schedule.addTask(queryButtonTask);
  schedule.addTask(updateDisplayTask);
  schedule.addTask(dataOutputTask);
  schedule.addTask(updateBaselineTask);
  schedule.addTask(resetPHCircuitTask);

  queryButtonTask.enable();
  resetPHCircuitTask.enable();
  updateBaselineTask.enable();
  writeDebugFileTimeTask.enableDelayed(60 * 60 * 1000);
  dataOutputTask.enableDelayed(OUT_REFRESH);
  updateDisplayTask.enableDelayed(DISPLAY_REFRESH);
}


/*
  LOOP FUNCTIONS
*/
void loop() {
  // Do nothing here

  /*
  const uint16_t DIG_THRESHOLD = 200;  // Digital threshold for the ADC

  if (sipm.available() > 0) {
    //events[event_position] = sipm.read();

    if (event_position >= EVENT_BUFFER - 1) {  // Increment if memory available, else overwrite array
      event_position = 0;
    } else {
      event_position++;
    }
  }
  */

  __wfi();  // Wait for interrupt
}


void loop1() {
  schedule.execute();

  delay(1);  // Wait for 1 ms, slightly reduces power consumption
}
