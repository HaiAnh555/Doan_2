#define BLYNK_TEMPLATE_ID   "TMPL2_WaXNEKa"
#define BLYNK_TEMPLATE_NAME "Parking"
#define BLYNK_AUTH_TOKEN    "rCgMhn7SjTs6bjYJw68mOspK1hVXWlsY"
#define BLYNK_PRINT Serial
#include <HTTPClient.h>

#include <WiFi.h>
#include <BlynkSimpleEsp32.h>

#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <SPI.h>
#include <MFRC522.h>
#include <ESP32Servo.h>

#define SERVER_URL "http://172.20.10.2:8000/api/event"

/************* WIFI *************/
#define WIFI_SSID "meomoccute"
#define WIFI_PASS "johnbeo55"

/************* LCD *************/
#define LCD_ADDR 0x27
LiquidCrystal_I2C lcd(LCD_ADDR, 16, 2);

/************* RFID *************/
#define RFID_SS   5
#define RFID_RST  4
MFRC522 rfid(RFID_SS, RFID_RST);

/************* IR SLOT PINS *************/
#define IR_SLOT1 34
#define IR_SLOT2 35
#define IR_SLOT3 32
#define IR_SLOT4 33
#define IR_ACTIVE_LOW true

/************* BUZZER *************/
#define BUZZER_PIN 14

/************* SERVO *************/
#define SERVO_ENTRY_PIN 13
#define SERVO_EXIT_PIN  27
Servo servoEntry;
Servo servoExit;

/************* TIMING *************/
static const uint32_t MSG_MS  = 1500;
static const uint32_t BOOT_MS = 2500;
static const uint32_t GATE_OPEN_MS = 1200;

uint32_t msgUntil  = 0;
uint32_t bootUntil = 0;
bool showingMsg = false;

/************* PARKING FEE *************/
static const uint32_t PRICE_PER_MIN = 3000;

/************* BLYNK PINS *************/
#define VPIN_SLOT1    V0
#define VPIN_SLOT2    V1
#define VPIN_SLOT3    V2
#define VPIN_SLOT4    V3
#define VPIN_MINUTES  V4   // so_phut_gui_xe
#define VPIN_MONEY    V5   // so_tien_phai_tra
#define VPIN_GATE_IN  V6   // Cong_vao (Switch)
#define VPIN_GATE_OUT V7   // Cong_ra  (Switch)

/************* LAST BILL *************/
uint32_t lastMinutes = 0;
uint32_t lastMoney   = 0;

/************* UID LIST + TIME *************/
#define MAX_UIDS 30
String insideUIDs[MAX_UIDS];
uint32_t insideEnterMs[MAX_UIDS];
int insideCount = 0;

/************* NON-BLOCKING BEEP *************/
struct {
  int beepsLeft = 0;
  int onMs = 0;
  int offMs = 0;
  bool phaseOn = false;
  uint32_t nextToggle = 0;
} beepSt;

void beepStart(int times, int onMs, int offMs) {
  if (times <= 0) return;
  beepSt.beepsLeft = times;
  beepSt.onMs = onMs;
  beepSt.offMs = offMs;
  beepSt.phaseOn = true;
  digitalWrite(BUZZER_PIN, HIGH);
  beepSt.nextToggle = millis() + (uint32_t)onMs;
}

void beepUpdate() {
  if (beepSt.beepsLeft <= 0) return;
  uint32_t now = millis();
  if ((int32_t)(now - beepSt.nextToggle) < 0) return;

  if (beepSt.phaseOn) {
    digitalWrite(BUZZER_PIN, LOW);
    beepSt.phaseOn = false;
    beepSt.nextToggle = now + (uint32_t)beepSt.offMs;
  } else {
    beepSt.beepsLeft--;
    if (beepSt.beepsLeft <= 0) {
      digitalWrite(BUZZER_PIN, LOW);
      return;
    }
    digitalWrite(BUZZER_PIN, HIGH);
    beepSt.phaseOn = true;
    beepSt.nextToggle = now + (uint32_t)beepSt.onMs;
  }
}

/************* NON-BLOCKING GATE *************/
struct {
  Servo* sv = nullptr;
  uint32_t startAt = 0;
  uint8_t stage = 0;
  uint32_t until = 0;
} gateSt;

void gateSchedule(Servo &sv, uint32_t startAt) {
  gateSt.sv = &sv;
  gateSt.startAt = startAt;
  gateSt.stage = 0;
  gateSt.until = 0;
}

void gateUpdate() {
  if (!gateSt.sv) return;
  uint32_t now = millis();

  if (gateSt.stage == 0) {
    if ((int32_t)(now - gateSt.startAt) >= 0) {
      gateSt.sv->write(180);            // mở
      gateSt.stage = 1;
      gateSt.until = now + GATE_OPEN_MS;
    }
  } else if (gateSt.stage == 1) {
    if ((int32_t)(now - gateSt.until) >= 0) {
      gateSt.sv->write(90);             // đóng
      gateSt.sv = nullptr;
      gateSt.stage = 0;
    }
  }
}

void cancelGateIfUsing(Servo &sv) {
  if (gateSt.sv == &sv) {
    gateSt.sv = nullptr;
    gateSt.stage = 0;
  }
}
void gateOpenCycleNow(Servo &sv) {
  cancelGateIfUsing(sv);
  gateSchedule(sv, millis());
}

/************* HELPERS *************/
bool readIR(int pin) {
  int v = digitalRead(pin);
  if (IR_ACTIVE_LOW) return (v == LOW);
  return (v == HIGH);
}

int freeSlots4(bool s1, bool s2, bool s3, bool s4) {
  int occ = (s1?1:0) + (s2?1:0) + (s3?1:0) + (s4?1:0);
  int free = 4 - occ;
  if (free < 0) free = 0;
  if (free > 4) free = 4;
  return free;
}

void printPadded(uint8_t col, uint8_t row, const String &s) {
  lcd.setCursor(col, row);
  String t = s;
  if (t.length() > 16) t = t.substring(0, 16);
  lcd.print(t);
  for (int i = t.length(); i < 16; i++) lcd.print(' ');
}

String slotLine4(bool s1, bool s2, bool s3, bool s4) {
  String line;
  line.reserve(16);
  line += "1:"; line += (s1 ? "F" : "E");
  line += " 2:"; line += (s2 ? "F" : "E");
  line += " 3:"; line += (s3 ? "F" : "E");
  line += " 4:"; line += (s4 ? "F" : "E");
  return line;
}

String uidToString() {
  String out = "UID:";
  for (byte i = 0; i < rfid.uid.size; i++) {
    out += (rfid.uid.uidByte[i] < 0x10) ? "0" : "";
    out += String(rfid.uid.uidByte[i], HEX);
    if (i < rfid.uid.size - 1) out += " ";
  }
  out.toUpperCase();
  return out;
}

int findUID(const String &uid) {
  for (int i = 0; i < insideCount; i++) if (insideUIDs[i] == uid) return i;
  return -1;
}

bool addUID(const String &uid) {
  if (insideCount >= MAX_UIDS) return false;
  insideUIDs[insideCount] = uid;
  insideEnterMs[insideCount] = millis();
  insideCount++;
  return true;
}

void removeUIDAt(int idx) {
  if (idx < 0 || idx >= insideCount) return;
  for (int i = idx; i < insideCount - 1; i++) {
    insideUIDs[i] = insideUIDs[i + 1];
    insideEnterMs[i] = insideEnterMs[i + 1];
  }
  insideCount--;
}

void sendEvent(const String &uid, const String &action,
               uint32_t minutes, uint32_t money) {
  if (WiFi.status() != WL_CONNECTED) return;

  // Đọc trạng thái slot thực tế tại thời điểm gửi event
  bool s1 = readIR(IR_SLOT1);
  bool s2 = readIR(IR_SLOT2);
  bool s3 = readIR(IR_SLOT3);
  bool s4 = readIR(IR_SLOT4);
  int free = freeSlots4(s1, s2, s3, s4);

  HTTPClient http;
  http.begin(SERVER_URL);
  http.addHeader("Content-Type", "application/json");

  String json = "{";
  json += "\"device_id\":\"esp32_parking_01\",";
  json += "\"uid\":\"" + uid + "\",";
  json += "\"action\":\"" + action + "\",";
  json += "\"minutes\":" + String((int)minutes) + ",";
  json += "\"money\":" + String((int)money) + ",";
  json += "\"slot1\":" + String(s1 ? 1 : 0) + ",";
  json += "\"slot2\":" + String(s2 ? 1 : 0) + ",";
  json += "\"slot3\":" + String(s3 ? 1 : 0) + ",";
  json += "\"slot4\":" + String(s4 ? 1 : 0) + ",";
  json += "\"free\":" + String(free);
  json += "}";

  http.POST(json);
  http.end();
}


/************* SCREENS *************/
void screenBoot(bool s1, bool s2, bool s3, bool s4) {
  int free = freeSlots4(s1, s2, s3, s4);
  printPadded(0, 0, "Bai do xe :v");
  printPadded(0, 1, "Con " + String(free) + " cho");
}

void screenStatus(bool s1, bool s2, bool s3, bool s4) {
  int free = freeSlots4(s1, s2, s3, s4);
  printPadded(0, 0, "Con " + String(free) + " cho");
  printPadded(0, 1, slotLine4(s1, s2, s3, s4));
}

void screenCardMsg(const String &uid, const String &msg) {
  printPadded(0, 0, uid);
  printPadded(0, 1, msg);
  showingMsg = true;
  msgUntil = millis() + MSG_MS;
}

void screenFee(uint32_t minutes, uint32_t money) {
  printPadded(0, 0, "Phut: " + String(minutes));
  printPadded(0, 1, "Tien: " + String(money) + "d");
  showingMsg = true;
  msgUntil = millis() + MSG_MS;
}

/************* BLYNK PUSH *************/
int lastV0 = -1, lastV1 = -1, lastV2 = -1, lastV3 = -1;

void blynkPushSlots(bool s1, bool s2, bool s3, bool s4) {
  int v0 = s1 ? 1 : 0;
  int v1 = s2 ? 1 : 0;
  int v2 = s3 ? 1 : 0;
  int v3 = s4 ? 1 : 0;

  if (v0 != lastV0) { Blynk.virtualWrite(VPIN_SLOT1, v0); lastV0 = v0; }
  if (v1 != lastV1) { Blynk.virtualWrite(VPIN_SLOT2, v1); lastV1 = v1; }
  if (v2 != lastV2) { Blynk.virtualWrite(VPIN_SLOT3, v2); lastV2 = v2; }
  if (v3 != lastV3) { Blynk.virtualWrite(VPIN_SLOT4, v3); lastV3 = v3; }
}

void blynkPushBill(uint32_t minutes, uint32_t money) {
  Blynk.virtualWrite(VPIN_MINUTES, (int)minutes);
  Blynk.virtualWrite(VPIN_MONEY,   (int)money);
}

/************* BLYNK CONTROL SERVO (V6/V7) *************/
BLYNK_WRITE(VPIN_GATE_IN) {
  int v = param.asInt();
  if (v == 1) {
    gateOpenCycleNow(servoEntry);
    Blynk.virtualWrite(VPIN_GATE_IN, 0);   // auto reset
  }
}
BLYNK_WRITE(VPIN_GATE_OUT) {
  int v = param.asInt();
  if (v == 1) {
    gateOpenCycleNow(servoExit);
    Blynk.virtualWrite(VPIN_GATE_OUT, 0);  // auto reset
  }
}

BLYNK_CONNECTED() {
  blynkPushBill(lastMinutes, lastMoney);
  Blynk.virtualWrite(VPIN_GATE_IN, 0);
  Blynk.virtualWrite(VPIN_GATE_OUT, 0);
}

void setup() {
  Serial.begin(9600);

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  pinMode(IR_SLOT1, INPUT);
  pinMode(IR_SLOT2, INPUT);
  pinMode(IR_SLOT3, INPUT);
  pinMode(IR_SLOT4, INPUT);

  Wire.begin(21, 22);
  lcd.init();
  lcd.backlight();

  SPI.begin(18, 19, 23, RFID_SS);
  rfid.PCD_Init();

  servoEntry.attach(SERVO_ENTRY_PIN, 500, 2400);
  servoEntry.write(90);

  servoExit.attach(SERVO_EXIT_PIN, 500, 2400);
  servoExit.write(90);

  Blynk.begin(BLYNK_AUTH_TOKEN, WIFI_SSID, WIFI_PASS);

  bool s1 = readIR(IR_SLOT1);
  bool s2 = readIR(IR_SLOT2);
  bool s3 = readIR(IR_SLOT3);
  bool s4 = readIR(IR_SLOT4);

  bootUntil = millis() + BOOT_MS;
  screenBoot(s1, s2, s3, s4);

  blynkPushSlots(s1, s2, s3, s4);

  lastMinutes = 0;
  lastMoney = 0;
  blynkPushBill(lastMinutes, lastMoney);

  Blynk.virtualWrite(VPIN_GATE_IN, 0);
  Blynk.virtualWrite(VPIN_GATE_OUT, 0);
}

void loop() {
  Blynk.run();
  beepUpdate();
  gateUpdate();

  bool s1 = readIR(IR_SLOT1);
  bool s2 = readIR(IR_SLOT2);
  bool s3 = readIR(IR_SLOT3);
  bool s4 = readIR(IR_SLOT4);

  blynkPushSlots(s1, s2, s3, s4);

  // Gửi trạng thái slot lên server liên tục để dashboard cập nhật realtime led
  static bool lastS1 = false, lastS2 = false, lastS3 = false, lastS4 = false;
  static uint32_t lastSend = 0;
  uint32_t now = millis();
  if (s1 != lastS1 || s2 != lastS2 || s3 != lastS3 || s4 != lastS4 || now - lastSend > 2000) {
    // Gửi event trạng thái slot (không cần UID/action/minutes/money)
    sendEvent("", "", 0, 0);
    lastS1 = s1; lastS2 = s2; lastS3 = s3; lastS4 = s4;
    lastSend = now;
  }

  static uint32_t lastCardMs = 0;

  if (!showingMsg && (millis() - lastCardMs > 700)) {
    if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
      lastCardMs = millis();

      String uidText = uidToString();
      Serial.println(uidText);

      // Danh sách UID hợp lệ
      const char* validUIDs[] = {"UID:5A 73 3D 02", "UID:6F 0A 20 1F", "UID:FF 9B 57 1E", "UID:D1 E1 FD 53" ,"UID:E1 64 A2 53"};
      const char* invalidUIDs[] = {"UID:2C D1 45 03"};
      bool isValid = false;
      for (int i = 0; i < 5; i++) {
        if (uidText == validUIDs[i]) {
          isValid = true;
          break;
        }
      }

      if (!isValid) {
        // Thẻ lạ: còi kêu 3 tiếng trong 1.5s, in LCD "thẻ lạ"
        beepStart(3, 150, 350); // Tổng 1.5s (3*(150+350)=1500ms)
        printPadded(0, 0, uidText);
        printPadded(0, 1, "the la");
        showingMsg = true;
        msgUntil = millis() + MSG_MS;
        rfid.PICC_HaltA();
        rfid.PCD_StopCrypto1();
        return;
      }

      int idx = findUID(uidText);

      if (idx == -1) {
        // THẺ VÀO
        // Kiểm tra bãi đã đầy chưa
        if (s1 && s2 && s3 && s4) {
          // Bãi đầy, không mở cổng, còi kêu 3 bíp, LCD: Da het cho
          beepStart(3, 150, 350); // 3 bíp trong 1.5s
          printPadded(0, 0, uidText);
          printPadded(0, 1, "Da het cho");
          showingMsg = true;
          msgUntil = millis() + MSG_MS;
        } else if (addUID(uidText)) {
          beepStart(2, 80, 60);
          sendEvent(uidText, "IN", 0, 0);
          screenCardMsg(uidText, "Welcome");
          gateSchedule(servoEntry, msgUntil);
        } else {
          beepStart(3, 120, 80);
          screenCardMsg("UID LIST FULL", "");
        }
      } else {
        // THẺ RA: TÍNH PHÚT + TIỀN
        uint32_t now = millis();
        uint32_t enterMs = insideEnterMs[idx];
        uint32_t elapsed = (now >= enterMs) ? (now - enterMs) : 0;

        uint32_t minutes = (elapsed + 60000UL - 1) / 60000UL;
        if (minutes == 0) minutes = 1;

        uint32_t money = minutes * PRICE_PER_MIN;

        beepStart(1, 120, 60);

        screenFee(minutes, money);

        // gửi lên Blynk (V4/V5)
        lastMinutes = minutes;
        lastMoney = money;
        sendEvent(uidText, "OUT", minutes, money);

        blynkPushBill(lastMinutes, lastMoney);

        gateSchedule(servoExit, msgUntil);

        removeUIDAt(idx);
      }

      rfid.PICC_HaltA();
      rfid.PCD_StopCrypto1();
    }
  }

  if (showingMsg) {
    if ((int32_t)(millis() - msgUntil) >= 0) showingMsg = false;
  } else {
    if ((int32_t)(millis() - bootUntil) < 0) screenBoot(s1, s2, s3, s4);
    else                                     screenStatus(s1, s2, s3, s4);
  }

  delay(5);
}
