// ===================================================================
// Smart Study Desk — Phase 1 firmware (rebuilt per full spec)
// Subjects: ACC, BS LAW, QA, ECO | Profile: CA THARUN | Exam: 41 days
// ===================================================================

#include <IRremote.hpp>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>
#include <time.h>
#include <WiFi.h>
#include <WebServer.h>
#include "BluetoothSerial.h"
#include <Preferences.h>

// ---------------- Pins ----------------
#define IR_RECEIVE_PIN 13
#define DHT_PIN        4
#define TRIG_PIN       27
#define ECHO_PIN       26
#define PIR_PIN        25
#define HALL_PIN       35   // analog
#define TOGGLE_SW_PIN  34   // freed up now that LDR is gone; logic not decided yet
#define PUSH_BTN_PIN   33
#define BUZZER_PIN     19
#define TOUCH_PIN      15
#define RELAY_PIN      23

#define STATUS_LED_R 16
#define STATUS_LED_G 17
#define STATUS_LED_B 18

#define AMBIENT_LED_R 5
#define AMBIENT_LED_G 2
#define AMBIENT_LED_B 0

// ---------------- IR remote codes ----------------
#define BTN_POWER 0xED127F80
#define BTN_MODE  0xE51A7F80
#define BTN_MUTE  0xE11E7F80
#define BTN_PLAY  0xFE017F80
#define BTN_PREV  0xFD027F80
#define BTN_NEXT  0xFC037F80
#define BTN_VOLDN 0xFA057F80
#define BTN_VOLUP 0xF9067F80
#define BTN_1     0xF50A7F80
#define BTN_2     0xE41B7F80
#define BTN_3     0xE01F7F80
#define BTN_4     0xF30C7F80
#define BTN_5     0xF20D7F80
#define BTN_6     0xF10E7F80
#define BTN_7     0xFF007F80
#define BTN_8     0xF00F7F80
#define BTN_9     0xE6197F80
#define BTN_EQ    0xFB047F80   // relay toggle

// ---------------- Tunable thresholds (calibrate on your desk) ----------------
int PRESENCE_DISTANCE_CM = 80;   // ultrasonic: "seated" if closer than this
int HALL_THRESHOLD       = 2000; // analog: phone-on-pad if reading crosses this — CALIBRATE with test sketch below
#define TOUCH_THRESHOLD 40       // calibrate with a standalone touchRead() print

#define STUDY_BLOCK_SECONDS 2700  // 45 min
#define BREAK_SECONDS       420   // 7 min

// ---------------- Objects ----------------
Adafruit_SSD1306 display(128, 64, &Wire, -1);
DHT dht(DHT_PIN, DHT11);
WebServer server(80);
BluetoothSerial SerialBT;
Preferences prefs;

const char* apSSID = "StudyDesk";
const char* apPassword = "study1234";
const char* hotspotSSID = "YOUR_PHONE_HOTSPOT_NAME";     // <-- fill in your phone's hotspot name
const char* hotspotPassword = "YOUR_PHONE_HOTSPOT_PASSWORD"; // <-- fill in its password
const char* profileName = "CA THARUN";

// ---------------- Subjects ----------------
#define NUM_SUBJECTS 4
const char* subjectNames[NUM_SUBJECTS] = {"ACC", "BS LAW", "QA", "ECO"};
unsigned long studySeconds[NUM_SUBJECTS] = {0};
int currentSubject = 0; // 0=ACC 1=BS LAW 2=QA 3=ECO

// ---------------- Session state machine ----------------
enum SessionState { IDLE, READY, STUDYING, DISTRACTED, ON_BREAK };
SessionState currentState = IDLE;

unsigned long blockElapsedSec = 0;   // counts toward 45-min block while STUDYING
unsigned long breakElapsedSec = 0;   // counts toward 7-min break while ON_BREAK
unsigned long lastSecondTick = 0;

// ---------------- Screens ----------------
enum ScreenPage { SCREEN_STUDY, SCREEN_ENV, SCREEN_PRESENCE, SCREEN_EXAM, SCREEN_COUNT };
ScreenPage currentScreen = SCREEN_STUDY;

// ---------------- Ambient LED mode ----------------
bool ledColorMode = false;
int presetColors[9][3] = {
  {255, 147, 41},   // 1 warm white
  {255, 255, 255},  // 2 cool white
  {255, 0, 0},      // 3 red
  {0, 255, 0},      // 4 green
  {0, 0, 255},      // 5 blue
  {255, 180, 0},    // 6 amber
  {160, 32, 240},   // 7 purple
  {0, 255, 255},    // 8 cyan
  {0, 0, 0}         // 9 off
};

// ---------------- Other state ----------------
bool dndActive = false;
bool relayOn = false;

bool greetedMorning = false, greetedLunch = false, greetedNight = false;
int examDaysRemaining = 41;
bool dayRolloverHandledToday = false;

struct ScheduleEntry { int hour, minute; const char* label; bool firedToday; };
ScheduleEntry schedule[] = {
  {9, 0, "ACC"},
  {11, 30, "BS LAW"},
  {14, 0, "QA"},
  {16, 30, "ECO"}
};
int scheduleCount = 4;

// ===================================================================
// SETUP
// ===================================================================
void setup() {
  Serial.begin(115200);

  IrReceiver.begin(IR_RECEIVE_PIN, ENABLE_LED_FEEDBACK);
  dht.begin();

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(PIR_PIN, INPUT);
  // HALL_PIN: analog, no pinMode needed
  pinMode(TOGGLE_SW_PIN, INPUT); // GPIO34 has no internal pulls — add external pull-up resistor in hardware
  pinMode(PUSH_BTN_PIN, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(RELAY_PIN, OUTPUT);

  pinMode(STATUS_LED_R, OUTPUT);
  pinMode(STATUS_LED_G, OUTPUT);
  pinMode(STATUS_LED_B, OUTPUT);
  pinMode(AMBIENT_LED_R, OUTPUT);
  pinMode(AMBIENT_LED_G, OUTPUT);
  pinMode(AMBIENT_LED_B, OUTPUT);

  setRelay(false); // tri-state method confirmed working on your hardware

  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  display.setRotation(2); // confirmed 180 degree mount
  display.clearDisplay();
  display.display();

  prefs.begin("studydesk", false);
  examDaysRemaining = prefs.getInt("examDays", 41); // 41 on first-ever boot, persisted after

  setupTimeFromSerial();
  setupWebServer();
  setupBluetooth();

  Serial.println("Smart Study Desk ready.");
}

// ===================================================================
// MAIN LOOP
// ===================================================================
void loop() {
  if (IrReceiver.decode()) {
    if (!(IrReceiver.decodedIRData.flags & IRDATA_FLAGS_IS_REPEAT)) {
      handleIRButton(IrReceiver.decodedIRData.decodedRawData);
    }
    IrReceiver.resume();
  }

  handleBluetooth();
  server.handleClient();

  updateSessionStateMachine();
  updateStudyAndBreakTimers();
  checkGreeting();
  checkSchedule();
  checkExamDayRollover();
  updateMainStatusLED();

  if (ledColorMode) {
    renderAmbientColorScreen();
  } else {
    renderScreen();
  }

  delay(200);
}

// ===================================================================
// IR remote handling
// ===================================================================
void handleIRButton(uint32_t code) {
  // In LED color mode, number buttons pick colors directly instead of subjects
  if (ledColorMode) {
    int idx = -1;
    switch (code) {
      case BTN_1: idx = 0; break;
      case BTN_2: idx = 1; break;
      case BTN_3: idx = 2; break;
      case BTN_4: idx = 3; break;
      case BTN_5: idx = 4; break;
      case BTN_6: idx = 5; break;
      case BTN_7: idx = 6; break;
      case BTN_8: idx = 7; break;
      case BTN_9: idx = 8; break;
      case BTN_MODE:
        ledColorMode = false;
        Serial.println("Back to normal mode");
        return;
    }
    if (idx >= 0) {
      setAmbientLED(presetColors[idx][0], presetColors[idx][1], presetColors[idx][2]);
      Serial.print("Ambient color set to preset "); Serial.println(idx + 1);
    }
    return;
  }

  // Normal mode
  switch (code) {
    case BTN_MODE:
      ledColorMode = true;
      Serial.println("Entered LED color mode — press 1-9 to pick a color");
      break;

    case BTN_POWER:
      if (currentState == IDLE) {
        currentState = READY;
        Serial.println("Session started — waiting for presence + phone on pad");
      } else {
        currentState = IDLE;
        Serial.println("Session stopped");
      }
      break;

    case BTN_PLAY:
      togglePause();
      break;

    case BTN_MUTE:
      dndActive = !dndActive;
      Serial.println(dndActive ? "DND ON (all alerts muted)" : "DND OFF");
      break;

    case BTN_PREV:
      currentScreen = (ScreenPage)((currentScreen == 0) ? SCREEN_COUNT - 1 : currentScreen - 1);
      break;

    case BTN_NEXT:
      currentScreen = (ScreenPage)((currentScreen + 1) % SCREEN_COUNT);
      break;

    case BTN_1: currentSubject = 0; break;
    case BTN_2: currentSubject = 1; break;
    case BTN_3: currentSubject = 2; break;
    case BTN_4: currentSubject = 3; break;

    case BTN_EQ:
      toggleRelay();
      Serial.println(relayOn ? "Relay ON (remote)" : "Relay OFF (remote)");
      break;

    // BTN_VOLUP/BTN_VOLDN — not assigned yet, free for future use
  }
}

void togglePause() {
  if (currentState == STUDYING) {
    currentState = READY; // pauses gating check until conditions naturally re-confirm, or user resumes
    Serial.println("Paused");
  } else if (currentState == READY || currentState == DISTRACTED) {
    Serial.println("Resume requested — will re-enter STUDYING once seated with phone on pad");
  }
}

// ===================================================================
// Session state machine
// Default gating (not fully specified yet): STUDYING requires
// PIR active AND ultrasonic distance < threshold AND Hall reading past threshold.
// Distraction is immediate — no grace period configured yet.
// ===================================================================
void updateSessionStateMachine() {
  bool present = isPresent();
  bool phoneOnPad = isPhoneOnPad();
  bool pushBtnPressed = (digitalRead(PUSH_BTN_PIN) == LOW); // duplicate of Play, active-low with pull-up

  if (pushBtnPressed) {
    togglePause();
    delay(300); // crude debounce
  }

  switch (currentState) {
    case IDLE:
      // waits for BTN_POWER
      break;

    case READY:
      if (present && phoneOnPad) {
        currentState = STUDYING;
        Serial.println("STUDYING started");
      }
      break;

    case STUDYING:
      if (!present || !phoneOnPad) {
        currentState = DISTRACTED;
        Serial.println("DISTRACTED — nagging");
      }
      break;

    case DISTRACTED:
      if (present && phoneOnPad) {
        currentState = STUDYING;
        Serial.println("Back to STUDYING");
      } else if (!dndActive) {
        tone(BUZZER_PIN, 3000, 150); // nag beep, repeats each loop pass while distracted
      }
      break;

    case ON_BREAK:
      // handled in updateStudyAndBreakTimers()
      break;
  }
}

bool isPresent() {
  bool pirActive = digitalRead(PIR_PIN);
  float dist = readDistanceCM();
  return pirActive && dist > 0 && dist < PRESENCE_DISTANCE_CM;
}

bool isPhoneOnPad() {
  int raw = analogRead(HALL_PIN);
  return raw > HALL_THRESHOLD; // CALIBRATE — direction may need flipping depending on your sensor
}

float readDistanceCM() {
  digitalWrite(TRIG_PIN, LOW); delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH); delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  long duration = pulseIn(ECHO_PIN, HIGH, 30000);
  if (duration == 0) return 999;
  return duration * 0.0343 / 2;
}

// ===================================================================
// Study timer + 45min/7min break cycle (fully automatic, no acknowledgment needed)
// ===================================================================
void updateStudyAndBreakTimers() {
  if (millis() - lastSecondTick < 1000) return;
  lastSecondTick = millis();

  if (currentState == STUDYING) {
    studySeconds[currentSubject]++;
    blockElapsedSec++;

    if (blockElapsedSec >= STUDY_BLOCK_SECONDS) {
      currentState = ON_BREAK;
      blockElapsedSec = 0;
      breakElapsedSec = 0;
      Serial.println("45 min done — auto BREAK started (7 min)");
      if (!dndActive) tone(BUZZER_PIN, 1500, 300);
    }
  } else if (currentState == ON_BREAK) {
    breakElapsedSec++;

    if (breakElapsedSec >= BREAK_SECONDS) {
      breakElapsedSec = 0;
      if (!dndActive) {
        for (int i = 0; i < 4; i++) {
          tone(BUZZER_PIN, 2000, 150);
          delay(200);
        }
      }
      // Auto-resume — re-enter READY, state machine will move to STUDYING once conditions confirm
      currentState = READY;
      Serial.println("Break over — resuming automatically");
    }
  }
}

// ===================================================================
// Relay control (remote EQ button, Bluetooth, web dashboard all share this)
// Tri-state method — confirmed working on your hardware
// ===================================================================
void setRelay(bool on) {
  if (on) {
    pinMode(RELAY_PIN, OUTPUT);
    digitalWrite(RELAY_PIN, LOW);
  } else {
    pinMode(RELAY_PIN, INPUT); // high-impedance, external pull-up brings IN to 5V = OFF
  }
  relayOn = on;
}

void toggleRelay() {
  setRelay(!relayOn);
}

// ===================================================================
// Ambient + status LEDs
// ===================================================================
void setAmbientLED(int r, int g, int b) {
  analogWrite(AMBIENT_LED_R, r);
  analogWrite(AMBIENT_LED_G, g);
  analogWrite(AMBIENT_LED_B, b);
}

void updateMainStatusLED() {
  switch (currentState) {
    case STUDYING:   setStatusColor(0, 255, 0); break;   // green
    case DISTRACTED:  setStatusColor(255, 0, 0); break;   // red
    case ON_BREAK:    setStatusColor(0, 0, 255); break;   // blue
    default:          setStatusColor(0, 0, 0); break;     // off — IDLE/READY
  }
}

void setStatusColor(int r, int g, int b) {
  analogWrite(STATUS_LED_R, r);
  analogWrite(STATUS_LED_G, g);
  analogWrite(STATUS_LED_B, b);
}

void renderAmbientColorScreen() {
  display.clearDisplay();
  display.setTextSize(2);
  display.setCursor(0, 20);
  display.println("LED MODE");
  display.setTextSize(1);
  display.setCursor(0, 45);
  display.println("Press 1-9 for color");
  display.display();
}

// ===================================================================
// OLED screens — normal mode, 2 items per screen, large text, browse with Prev/Next
// ===================================================================
void renderScreen() {
  display.clearDisplay();

  switch (currentScreen) {
    case SCREEN_STUDY: {
      display.setTextSize(2);
      display.setCursor(0, 5);
      display.println(sessionStateName(currentState));
      display.setTextSize(1);
      display.setCursor(0, 35);
      display.print("Subject: ");
      display.println(subjectNames[currentSubject]);
      display.setCursor(0, 50);
      display.print("Today: ");
      display.print(studySeconds[currentSubject] / 60);
      display.println(" min");
      break;
    }
    case SCREEN_ENV: {
      float t = dht.readTemperature();
      float h = dht.readHumidity();
      display.setTextSize(2);
      display.setCursor(0, 10);
      display.print(t, 1); display.println(" C");
      display.setCursor(0, 38);
      display.print(h, 1); display.println(" %RH");
      break;
    }
    case SCREEN_PRESENCE: {
      display.setTextSize(2);
      display.setCursor(0, 10);
      display.println(isPresent() ? "SEATED" : "AWAY");
      display.setTextSize(1);
      display.setCursor(0, 40);
      display.print("Phone: ");
      display.println(isPhoneOnPad() ? "on pad" : "away");
      break;
    }
    case SCREEN_EXAM: {
      display.setTextSize(2);
      display.setCursor(0, 5);
      display.print(examDaysRemaining);
      display.println(" days");
      display.setTextSize(1);
      display.setCursor(0, 40);
      display.println("to exam");
      break;
    }
    default: break;
  }
  display.display();
}

const char* sessionStateName(SessionState s) {
  switch (s) {
    case IDLE: return "IDLE";
    case READY: return "READY";
    case STUDYING: return "STUDYING";
    case DISTRACTED: return "DISTRACTED";
    case ON_BREAK: return "BREAK";
    default: return "?";
  }
}

// ===================================================================
// Time-of-day (manual set once per boot, ESP32 tracks it internally after)
// ===================================================================
void setupTimeFromSerial() {
  Serial.println("Enter current time as HH:MM (24hr), then press Enter:");
  while (!Serial.available()) { delay(100); }
  String input = Serial.readStringUntil('\n');
  int hh = input.substring(0, 2).toInt();
  int mm = input.substring(3, 5).toInt();

  struct tm t = {0};
  t.tm_hour = hh; t.tm_min = mm; t.tm_sec = 0;
  t.tm_mday = 1; t.tm_mon = 0; t.tm_year = 125;

  time_t epoch = mktime(&t);
  struct timeval now = { .tv_sec = epoch };
  settimeofday(&now, NULL);
  Serial.println("Time set.");
}

void getCurrentTime(int &hour, int &minute) {
  time_t now; time(&now);
  struct tm *t = localtime(&now);
  hour = t->tm_hour; minute = t->tm_min;
}

// ===================================================================
// Touch + presence greeting — single profile: CA THARUN
// ===================================================================
void checkGreeting() {
  bool touched = touchRead(TOUCH_PIN) < TOUCH_THRESHOLD;
  if (!touched || !isPresent()) return;

  int hour, minute;
  getCurrentTime(hour, minute);

  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 15);

  if (hour < 6 && !greetedMorning) {
    display.print("Good morning, "); display.println(profileName);
    display.println("Have a great day ahead!");
    greetedMorning = true;
  } else if (hour == 13 && minute >= 25 && minute <= 35 && !greetedLunch) {
    display.println("Lunch break time!");
    display.println("Recharge and come back strong");
    greetedLunch = true;
  } else if (hour == 22 && !greetedNight) {
    display.print("Good night, "); display.println(profileName);
    display.println("Rest well, see you tomorrow");
    greetedNight = true;
  } else {
    display.print("Hi, "); display.println(profileName);
  }

  display.display();
  delay(2000);

  if (hour == 0 && minute == 0) {
    greetedMorning = greetedLunch = greetedNight = false;
  }
}

// ===================================================================
// Exam day countdown — persisted in flash, decrements once per detected midnight
// LIMITATION: if powered off across midnight, that day won't auto-decrement.
// Use Bluetooth command "DAYS n" to correct manually if needed.
// ===================================================================
void checkExamDayRollover() {
  int hour, minute;
  getCurrentTime(hour, minute);

  if (hour == 0 && minute == 0) {
    if (!dayRolloverHandledToday) {
      examDaysRemaining = max(0, examDaysRemaining - 1);
      prefs.putInt("examDays", examDaysRemaining);
      dayRolloverHandledToday = true;
      Serial.print("Exam days remaining: "); Serial.println(examDaysRemaining);
    }
  } else {
    dayRolloverHandledToday = false;
  }
}

// ===================================================================
// Scheduled subject reminders (buzzer beep) — placeholder times, edit via dashboard later
// ===================================================================
void checkSchedule() {
  if (dndActive) return;

  int hour, minute;
  getCurrentTime(hour, minute);

  for (int i = 0; i < scheduleCount; i++) {
    if (hour == schedule[i].hour && minute == schedule[i].minute && !schedule[i].firedToday) {
      tone(BUZZER_PIN, 2000, 500);
      schedule[i].firedToday = true;
      Serial.print("Reminder: "); Serial.println(schedule[i].label);
    }
    if (hour == 0 && minute == 0) schedule[i].firedToday = false;
  }
}

// ===================================================================
// WiFi web dashboard — ESP32 hosts its own AP, fully offline
// Separate pages per section, matching the OLED's screen-by-screen approach
// ===================================================================
void setupWebServer() {
  Serial.print("Trying to join hotspot '");
  Serial.print(hotspotSSID);
  Serial.println("'...");

  WiFi.mode(WIFI_STA);
  WiFi.begin(hotspotSSID, hotspotPassword);

  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 10000) {
    delay(300);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.print("Connected to hotspot. Dashboard at: http://");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println();
    Serial.println("Hotspot not found — hosting ESP32's own WiFi network instead.");
    WiFi.mode(WIFI_AP);
    WiFi.softAP(apSSID, apPassword);
    Serial.print("Connect your phone to '");
    Serial.print(apSSID);
    Serial.print("' then visit http://");
    Serial.println(WiFi.softAPIP());
  }

  server.on("/", handleHome);
  server.on("/env", handleEnv);
  server.on("/study", handleStudy);
  server.on("/exam", handleExam);
  server.on("/relay/on", [](){ setRelay(true); server.sendHeader("Location", "/"); server.send(303); });
  server.on("/relay/off", [](){ setRelay(false); server.sendHeader("Location", "/"); server.send(303); });
  server.begin();
}

String pageWrapper(String title, String activePage, String body) {
  String html = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>Study Desk</title><style>";
  html += ":root{--ink:#1B2430;--panel:#232F3E;--brass:#C9A227;--teal:#3E6259;--parchment:#F2EFE9;--red:#C1443A;--blue:#3E6B8A;--muted:#8B94A3;}";
  html += "*{box-sizing:border-box;}";
  html += "body{margin:0;background:var(--ink);color:var(--parchment);font-family:-apple-system,Segoe UI,Roboto,sans-serif;padding:24px;}";
  html += "h1{font-family:Georgia,'Times New Roman',serif;font-weight:400;font-size:28px;letter-spacing:0.5px;margin:0 0 4px 0;color:var(--parchment);}";
  html += ".eyebrow{color:var(--brass);font-size:12px;letter-spacing:2px;text-transform:uppercase;margin-bottom:20px;}";
  html += "nav{display:flex;gap:4px;margin-bottom:28px;border-bottom:1px solid #33404F;padding-bottom:0;flex-wrap:wrap;}";
  html += "nav a{color:var(--muted);text-decoration:none;font-size:14px;padding:10px 16px;border-bottom:2px solid transparent;}";
  html += "nav a.active{color:var(--brass);border-bottom:2px solid var(--brass);}";
  html += ".card{background:var(--panel);border-radius:10px;padding:20px 24px;margin-bottom:16px;border:1px solid #2E3B4C;}";
  html += ".big-number{font-family:'Courier New',monospace;font-size:56px;color:var(--brass);line-height:1;}";
  html += ".label{color:var(--muted);font-size:13px;text-transform:uppercase;letter-spacing:1px;margin-bottom:6px;}";
  html += ".ring{width:120px;height:120px;border-radius:50%;display:flex;align-items:center;justify-content:center;font-family:Georgia,serif;font-size:15px;border:4px solid;margin:0 auto 12px auto;}";
  html += ".btn{display:inline-block;background:var(--teal);color:var(--parchment);text-decoration:none;padding:12px 22px;border-radius:6px;font-size:14px;margin-right:10px;border:1px solid #4A7168;}";
  html += ".btn.off{background:transparent;color:var(--muted);border:1px solid #3A4757;}";
  html += "table{width:100%;border-collapse:collapse;}";
  html += "td,th{padding:10px 8px;text-align:left;border-bottom:1px solid #2E3B4C;font-size:14px;}";
  html += "th{color:var(--muted);font-weight:400;text-transform:uppercase;font-size:11px;letter-spacing:1px;}";
  html += "</style></head><body>";
  html += "<div class='eyebrow'>Smart Study Desk &middot; CA THARUN</div>";
  html += "<h1>" + title + "</h1>";
  html += "<nav>";
  html += "<a href='/' class='" + String(activePage == "home" ? "active" : "") + "'>Status</a>";
  html += "<a href='/env' class='" + String(activePage == "env" ? "active" : "") + "'>Environment</a>";
  html += "<a href='/study' class='" + String(activePage == "study" ? "active" : "") + "'>Study hours</a>";
  html += "<a href='/exam' class='" + String(activePage == "exam" ? "active" : "") + "'>Exam countdown</a>";
  html += "</nav>";
  html += body;
  html += "</body></html>";
  return html;
}

String ringColorFor(SessionState s) {
  switch (s) {
    case STUDYING: return "var(--teal)";
    case DISTRACTED: return "var(--red)";
    case ON_BREAK: return "var(--blue)";
    default: return "var(--muted)";
  }
}

void handleHome() {
  String body = "<div class='card' style='text-align:center'>";
  body += "<div class='ring' style='border-color:" + ringColorFor(currentState) + ";color:" + ringColorFor(currentState) + "'>" + String(sessionStateName(currentState)) + "</div>";
  body += "<div class='label'>Current subject</div>";
  body += "<div style='font-size:20px;margin-bottom:18px'>" + String(subjectNames[currentSubject]) + "</div>";
  body += "</div>";

  body += "<div class='card'>";
  body += "<div class='label'>Charger socket</div>";
  body += "<div style='font-size:22px;margin-bottom:14px'>" + String(relayOn ? "ON" : "OFF") + "</div>";
  body += "<a href='/relay/on' class='btn'>Turn on</a>";
  body += "<a href='/relay/off' class='btn off'>Turn off</a>";
  body += "</div>";

  server.send(200, "text/html", pageWrapper("Desk status", "home", body));
}

void handleEnv() {
  String body = "<div class='card'><div class='label'>Temperature</div><div class='big-number'>" + String(dht.readTemperature(), 1) + "&deg;</div></div>";
  body += "<div class='card'><div class='label'>Humidity</div><div class='big-number'>" + String(dht.readHumidity(), 1) + "%</div></div>";
  body += "<div class='card'><div class='label'>Presence</div><div style='font-size:20px'>" + String(isPresent() ? "Seated at desk" : "Away") + "</div>";
  body += "<div class='label' style='margin-top:12px'>Phone</div><div style='font-size:20px'>" + String(isPhoneOnPad() ? "On pad" : "Away") + "</div></div>";
  server.send(200, "text/html", pageWrapper("Environment", "env", body));
}

void handleStudy() {
  String body = "<div class='card'><table><tr><th>Subject</th><th>Minutes today</th></tr>";
  for (int i = 0; i < NUM_SUBJECTS; i++) {
    body += "<tr><td>" + String(subjectNames[i]) + "</td><td>" + String(studySeconds[i] / 60) + "</td></tr>";
  }
  body += "</table></div>";
  server.send(200, "text/html", pageWrapper("Study hours", "study", body));
}

void handleExam() {
  String body = "<div class='card' style='text-align:center'>";
  body += "<div class='big-number' style='font-size:72px'>" + String(examDaysRemaining) + "</div>";
  body += "<div class='label'>days remaining to your exam</div>";
  body += "</div>";
  server.send(200, "text/html", pageWrapper("Exam countdown", "exam", body));
}

// ===================================================================
// Bluetooth control — pairs as "StudyDesk"
// Commands: RELAY_ON, RELAY_OFF, RELAY_TOGGLE, DAYS <n>
// ===================================================================
void setupBluetooth() {
  SerialBT.begin("StudyDesk");
  Serial.println("Bluetooth ready — pair with 'StudyDesk'");
}

void handleBluetooth() {
  if (!SerialBT.available()) return;
  String cmd = SerialBT.readStringUntil('\n');
  cmd.trim();

  if (cmd == "RELAY_ON") setRelay(true);
  else if (cmd == "RELAY_OFF") setRelay(false);
  else if (cmd == "RELAY_TOGGLE") toggleRelay();
  else if (cmd.startsWith("DAYS")) {
    examDaysRemaining = cmd.substring(4).toInt();
    prefs.putInt("examDays", examDaysRemaining);
  }
  SerialBT.println("Relay:" + String(relayOn ? "ON" : "OFF") + " Days:" + String(examDaysRemaining));
}

// ===================================================================
// STANDALONE HALL SENSOR TEST — copy into a separate blank sketch to calibrate
// (kept here as reference only, not called from loop())
// ===================================================================
/*
#define HALL_PIN 35
void setup() { Serial.begin(115200); }
void loop() {
  int raw = analogRead(HALL_PIN);
  Serial.println(raw);
  delay(200);
}
// Place your phone (with magnet) on the pad -> note the reading.
// Remove it -> note the reading.
// Set HALL_THRESHOLD in the main sketch roughly halfway between the two.
// If it reads backwards (higher when phone is AWAY), flip the > to < in isPhoneOnPad().
*/
