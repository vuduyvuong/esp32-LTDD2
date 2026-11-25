#include <SPI.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ESP32Servo.h>

// --- CHÂN KẾT NỐI ---
#define SS_PIN    5   // GPIO 5 (SDA của RC522)
#define RST_PIN   4   // GPIO 4 (RST của RC522)
#define SERVO_PIN 13  // GPIO 13
#define MQ2_PIN   34  // GPIO 34
#define FLAME_PIN 35  // GPIO 35
#define LED_RED   25  // GPIO 25
#define LED_GREEN 26  // GPIO 26
#define BUZZER    27  // GPIO 27
#define BTN_PIN   32  // GPIO 32

LiquidCrystal_I2C lcd(0x27, 16, 2);
Servo barrierServo;

// Mã thẻ hợp lệ (4 byte)
byte validUID[4] = {0xDE, 0xAD, 0xBE, 0xEF};
byte readUID[4];

bool gateOpen = false;
unsigned long gateOpenTime = 0;
const unsigned long GATE_DELAY = 3000;

// --- DRIVER RC522 THUẦN (KHÔNG CẦN THƯ VIỆN NGOÀI) ---
void rc522Write(byte reg, byte val) {
  digitalWrite(SS_PIN, LOW);
  SPI.transfer((reg << 1) & 0x7E);
  SPI.transfer(val);
  digitalWrite(SS_PIN, HIGH);
}

byte rc522Read(byte reg) {
  digitalWrite(SS_PIN, LOW);
  SPI.transfer(((reg << 1) & 0x7E) | 0x80);
  byte val = SPI.transfer(0x00);
  digitalWrite(SS_PIN, HIGH);
  return val;
}

void rc522Init() {
  pinMode(SS_PIN, OUTPUT);
  pinMode(RST_PIN, OUTPUT);
  digitalWrite(SS_PIN, HIGH);
  digitalWrite(RST_PIN, HIGH);
  rc522Write(0x01, 0x0F); // SoftReset
  delay(50);
  rc522Write(0x2A, 0x8D); // TModeReg
  rc522Write(0x2B, 0x3E); // TPrescalerReg
  rc522Write(0x2D, 30);   // TReloadRegL
  rc522Write(0x2C, 0);    // TReloadRegH
  rc522Write(0x15, 0x40); // TxASKReg
  rc522Write(0x11, 0x3D); // ModeReg
  rc522Write(0x14, rc522Read(0x14) | 0x03); // AntennaOn
}

bool rc522Communicate(byte command, byte *sendData, byte sendLen, byte *backData, byte *backLen) {
  byte irqEn = (command == 0x0C) ? 0x77 : 0x00;
  byte waitIRq = (command == 0x0C) ? 0x30 : 0x00;

  rc522Write(0x02, irqEn | 0x80);
  rc522Write(0x04, rc522Read(0x04) & ~0x80);
  rc522Write(0x0A, rc522Read(0x0A) | 0x80);
  rc522Write(0x01, 0x00);

  for (byte i = 0; i < sendLen; i++) rc522Write(0x09, sendData[i]);
  rc522Write(0x01, command);
  if (command == 0x0C) rc522Write(0x0D, rc522Read(0x0D) | 0x80);

  unsigned int i = 2000;
  byte n;
  do {
    n = rc522Read(0x04);
    i--;
  } while ((i != 0) && !(n & 0x01) && !(n & waitIRq));

  rc522Write(0x0D, rc522Read(0x0D) & ~0x80);
  if (i != 0 && !(rc522Read(0x06) & 0x1B)) {
    if (command == 0x0C) {
      n = rc522Read(0x0A);
      byte lastBits = rc522Read(0x0C) & 0x07;
      *backLen = lastBits ? (n - 1) * 8 + lastBits : n * 8;
      if (n == 0) n = 1;
      if (n > 16) n = 16;
      for (byte j = 0; j < n; j++) backData[j] = rc522Read(0x09);
    }
    return true;
  }
  return false;
}

bool checkRFIDCard() {
  byte buffer[18];
  byte len;
  rc522Write(0x0D, 0x07);
  byte req[1] = {0x26}; // PICC_REQIDL
  if (!rc522Communicate(0x0C, req, 1, buffer, &len) || len != 0x10) return false;

  rc522Write(0x0D, 0x00);
  byte antiColl[2] = {0x93, 0x20};
  if (!rc522Communicate(0x0C, antiColl, 2, buffer, &len)) return false;

  for (byte i = 0; i < 4; i++) readUID[i] = buffer[i];
  return true;
}

// --- CHƯƠNG TRÌNH CHÍNH ---
void setup() {
  Serial.begin(115200);

  pinMode(MQ2_PIN, INPUT);
  pinMode(FLAME_PIN, INPUT);
  pinMode(LED_RED, OUTPUT);
  pinMode(LED_GREEN, OUTPUT);
  pinMode(BUZZER, OUTPUT);
  pinMode(BTN_PIN, INPUT_PULLUP);

  digitalWrite(LED_RED, HIGH);
  digitalWrite(LED_GREEN, LOW);
  digitalWrite(BUZZER, LOW);

  Wire.begin(21, 22);
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("HE THONG SAN SANG");
  lcd.setCursor(0, 1);
  lcd.print("MOI QUET THE...");

  SPI.begin();
  rc522Init();

  barrierServo.attach(SERVO_PIN);
  barrierServo.write(0);
}

void loop() {
  bool isGasDetected = (digitalRead(MQ2_PIN) == LOW);
  bool isFlameDetected = (digitalRead(FLAME_PIN) == LOW);

  if (isGasDetected || isFlameDetected) {
    handleEmergency(isGasDetected, isFlameDetected);
    return;
  }

  if (digitalRead(BTN_PIN) == LOW) {
    delay(50);
    if (digitalRead(BTN_PIN) == LOW) {
      openGate("MO CONG THU CONG");
    }
  }

  if (checkRFIDCard()) {
    Serial.print("UID The: ");
    bool isAuthorized = true;
    for (byte i = 0; i < 4; i++) {
      Serial.print(readUID[i] < 0x10 ? " 0" : " ");
      Serial.print(readUID[i], HEX);
      if (readUID[i] != validUID[i]) isAuthorized = false;
    }
    Serial.println();

    if (isAuthorized) {
      openGate("THE HOP LE!");
    } else {
      denyAccess();
    }
    delay(500);
  }

  if (gateOpen && (millis() - gateOpenTime >= GATE_DELAY)) {
    closeGate();
  }
}

void openGate(String reason) {
  gateOpen = true;
  gateOpenTime = millis();
  digitalWrite(LED_RED, LOW);
  digitalWrite(LED_GREEN, HIGH);
  digitalWrite(BUZZER, HIGH);
  delay(100);
  digitalWrite(BUZZER, LOW);

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(reason);
  lcd.setCursor(0, 1);
  lcd.print("MO THANH CHAN...");
  barrierServo.write(90);
}

void closeGate() {
  gateOpen = false;
  digitalWrite(LED_GREEN, LOW);
  digitalWrite(LED_RED, HIGH);
  barrierServo.write(0);

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("HE THONG SAN SANG");
  lcd.setCursor(0, 1);
  lcd.print("MOI QUET THE...");
}

void denyAccess() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("THE KHONG HOP LE");
  lcd.setCursor(0, 1);
  lcd.print("TU CHOI TRUY CAP");

  for (int i = 0; i < 3; i++) {
    digitalWrite(LED_RED, LOW);
    digitalWrite(BUZZER, HIGH);
    delay(150);
    digitalWrite(LED_RED, HIGH);
    digitalWrite(BUZZER, LOW);
    delay(150);
  }
  delay(1000);
  closeGate();
}

void handleEmergency(bool gas, bool flame) {
  barrierServo.write(90);
  gateOpen = true;

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("CANH BAO NGUY HIEM");
  lcd.setCursor(0, 1);
  if (gas && flame) lcd.print("CO LUA & KHI GAS!");
  else if (flame) lcd.print("PHAT HIEN LUA!");
  else lcd.print("RO RI KHI GAS!");

  digitalWrite(LED_GREEN, LOW);
  digitalWrite(LED_RED, HIGH);
  digitalWrite(BUZZER, HIGH);
  delay(200);
  digitalWrite(LED_RED, LOW);
  digitalWrite(BUZZER, LOW);
  delay(200);
}