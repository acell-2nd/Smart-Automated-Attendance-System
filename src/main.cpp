/*
 * Smart Automated Attendance System (simulasi Wokwi + PlatformIO, ESP32)
 *
 * Rantai logika 3 lapis:
 *   PIR (wake-up) -> Ultrasonik (kunci jarak ideal) -> RFID (tap kartu)
 * Kamera dan WhatsApp di-mock lewat Serial Monitor.
 *
 * Setiap absen yang berhasil mencetak 1 baris CSV ke Serial Monitor:
 *   CSV,<uid>,<nama>,<tanggal>,<jam>
 * Data ini nanti dipakai untuk clustering kedisiplinan karyawan.
 *
 * Cara memilih karyawan saat simulasi (selain tap kartu virtual di komponen MFRC522):
 *   ketik angka 1-5 di Serial Monitor lalu Enter saat posisi sudah LOCKED
 *   -> dianggap karyawan nomor tersebut men-tap kartunya.
 *
 * Catatan: STANDBY di simulasi memakai flag state. Di perangkat asli diganti
 * deep sleep ESP32 dengan wake-up dari PIR (ext0 di pin RTC, GPIO 27).
 */

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <LiquidCrystal_I2C.h>
#include <MFRC522.h>

// ===================== PIN =====================
#define PIR_PIN     27
#define TRIG_PIN    16
#define ECHO_PIN    17
#define LED_GREEN   25
#define LED_RED     26
#define SDA_PIN     21
#define SCL_PIN     22
#define RFID_SS     5
#define RFID_RST    33
#define RFID_SCK    18
#define RFID_MISO   19
#define RFID_MOSI   23

// ===================== PARAMETER =====================
const float IDEAL_MIN_CM        = 30.0;   // batas jarak ideal (cm)
const float IDEAL_MAX_CM        = 40.0;
const float LOCK_TOLERANCE_CM   = 3.0;    // toleransi supaya LOCKED tidak berkedip
const float PRESENCE_MAX_CM     = 150.0;  // di atas ini dianggap tidak ada orang
const unsigned long IDLE_TIMEOUT_MS     = 5000;  // 5 detik tanpa deteksi -> tidur
const unsigned long MEASURE_INTERVAL_MS = 150;
const unsigned long RESULT_SHOW_MS      = 3000;  // lama pesan hasil tampil

// ===================== JAM VIRTUAL =====================
// Jam mulai 07:00:00. TIME_SCALE = berapa detik virtual per 1 detik nyata.
// 60 -> 1 detik nyata = 1 menit virtual, jadi tap di waktu berbeda menghasilkan jam masuk berbeda.
// Ubah ke 1 kalau mau jam berjalan normal. Tanggal tetap, silakan ubah di bawah.
#define TIME_SCALE  60
#define TANGGAL     "2026-10-12"

// ===================== DATA KARYAWAN (DUMMY) =====================
// Ganti UID dengan UID yang tercetak saat kamu tap kartu virtual di MFRC522
// (format: "AA BB CC DD", huruf besar). Untuk pilihan lewat Serial, UID tidak perlu diubah.
struct User {
  const char* uid;
  const char* nama;
  const char* wa;
};

User users[] = {
  {"DE AD BE EF", "Budi Santoso", "0812-0000-0001"},
  {"01 02 03 04", "Siti Aminah",  "0812-0000-0002"},
  {"AA BB CC DD", "Andi Pratama", "0812-0000-0003"},
  {"11 22 33 44", "Dewi Lestari", "0812-0000-0004"},
  {"55 66 77 88", "Rudi Hartono", "0812-0000-0005"},
};
const int USER_COUNT = sizeof(users) / sizeof(users[0]);

// ===================== OBJEK & STATE =====================
LiquidCrystal_I2C lcd(0x27, 16, 2);
MFRC522 mfrc522(RFID_SS, RFID_RST);

enum State { STANDBY, POSITIONING, LOCKED, TRANSAKSI };
State state = STANDBY;

float distanceCm = -1;
unsigned long lastMeasure = 0;
unsigned long lastSeen = 0;
unsigned long resultUntil = 0;
int fotoCounter = 0;
String cache1 = "", cache2 = "";

// ===================== HELPER =====================
String pad16(const String& s) {
  String r = s.substring(0, 16);
  while (r.length() < 16) r += ' ';
  return r;
}

// Tulis ke LCD hanya jika teks berubah (mencegah berkedip)
void show(const String& a, const String& b) {
  String l1 = pad16(a);
  String l2 = pad16(b);
  if (l1 != cache1) { lcd.setCursor(0, 0); lcd.print(l1); cache1 = l1; }
  if (l2 != cache2) { lcd.setCursor(0, 1); lcd.print(l2); cache2 = l2; }
}

void setLeds(bool green, bool red) {
  digitalWrite(LED_GREEN, green ? HIGH : LOW);
  digitalWrite(LED_RED, red ? HIGH : LOW);
}

float readDistanceCm() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  long dur = pulseIn(ECHO_PIN, HIGH, 30000);  // timeout 30 ms
  if (dur == 0) return -1;                    // tidak ada echo
  return dur * 0.0343 / 2.0;
}

// Jam virtual "HH:MM:SS" (mulai 07:00:00, berjalan sesuai TIME_SCALE)
String jamSekarang() {
  unsigned long s = 7UL * 3600UL + (millis() / 1000UL) * TIME_SCALE;
  char buf[9];
  snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu", (s / 3600) % 24, (s / 60) % 60, s % 60);
  return String(buf);
}

String uidToString() {
  String s = "";
  for (byte i = 0; i < mfrc522.uid.size; i++) {
    if (i > 0) s += ' ';
    if (mfrc522.uid.uidByte[i] < 0x10) s += '0';
    s += String(mfrc522.uid.uidByte[i], HEX);
  }
  s.toUpperCase();
  return s;
}

// Ada kartu di-tap? Bisa lewat MFRC522 (kartu virtual) atau ketik 1-5 di Serial Monitor.
bool readCardUid(String& uid) {
  while (Serial.available() > 0) {
    int c = Serial.read();
    if (c >= '1' && c < '1' + USER_COUNT) {
      uid = String(users[c - '1'].uid);
      return true;
    }
  }
  if (!mfrc522.PICC_IsNewCardPresent()) return false;
  if (!mfrc522.PICC_ReadCardSerial()) return false;
  uid = uidToString();
  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();
  return true;
}

// ===================== TRANSISI STATE =====================
void enterStandby() {
  state = STANDBY;
  setLeds(false, false);
  lcd.clear();
  lcd.noBacklight();
  cache1 = "";
  cache2 = "";
  Serial.println("[STATE] STANDBY (layar, LED, sensor jarak & RFID nonaktif)");
}

void wakeUp() {
  lcd.backlight();
  lcd.clear();
  cache1 = "";
  cache2 = "";
  state = POSITIONING;
  lastSeen = millis();
  lastMeasure = 0;
  Serial.println("[STATE] WAKE UP oleh PIR -> POSITIONING");
}

// ===================== TRANSAKSI =====================
void processCard(const String& uid) {
  Serial.print("[RFID] UID terbaca: ");
  Serial.println(uid);

  int idx = -1;
  for (int i = 0; i < USER_COUNT; i++) {
    if (uid == users[i].uid) { idx = i; break; }
  }

  if (idx >= 0) {
    fotoCounter++;
    char foto[16];
    snprintf(foto, sizeof(foto), "IMG_%03d.jpg", fotoCounter);
    String jam = jamSekarang();

    Serial.printf("[KAMERA] Foto diambil: %s\n", foto);
    Serial.printf("[DB] Absen tercatat: %s | UID %s | %s\n", users[idx].nama, uid.c_str(), jam.c_str());
    Serial.printf("[WA] Ke %s: Halo %s, absen masuk berhasil pukul %s. Foto terlampir: %s\n",
                  users[idx].wa, users[idx].nama, jam.c_str(), foto);
    // Baris data untuk dataset CSV
    Serial.printf("CSV,%s,%s,%s,%s\n", uid.c_str(), users[idx].nama, TANGGAL, jam.c_str());

    show("Absen Berhasil!", users[idx].nama);
    setLeds(true, false);
  } else {
    Serial.println("[RFID] Kartu tidak terdaftar, absen ditolak");
    show("Kartu Tidak", "Terdaftar");
    setLeds(false, true);
  }

  state = TRANSAKSI;
  resultUntil = millis() + RESULT_SHOW_MS;
  lastSeen = millis();
}

// ===================== HANDLER PER STATE =====================
void handlePositioning() {
  bool valid = (distanceCm > 0);

  if (valid && distanceCm >= IDEAL_MIN_CM && distanceCm <= IDEAL_MAX_CM) {
    state = LOCKED;
    Serial.printf("[STATE] LOCKED pada jarak %.1f cm\n", distanceCm);
    return;
  }

  if (!valid || distanceCm > PRESENCE_MAX_CM) {
    show("Selamat Datang", "Silakan Mendekat");
    setLeds(false, false);
  } else if (distanceCm > IDEAL_MAX_CM) {
    show("Silakan Maju", "Jarak: " + String((int)round(distanceCm)) + " cm");
    setLeds(false, true);
  } else {
    show("Terlalu Dekat", "Mundur sedikit");
    setLeds(false, true);
  }
}

void handleLocked() {
  // Keluar dari kunci jika pengguna bergeser terlalu jauh dari jarak ideal
  if (distanceCm < 0 ||
      distanceCm < IDEAL_MIN_CM - LOCK_TOLERANCE_CM ||
      distanceCm > IDEAL_MAX_CM + LOCK_TOLERANCE_CM) {
    state = POSITIONING;
    Serial.println("[STATE] Posisi bergeser -> POSITIONING");
    return;
  }

  show("Posisi Sempurna", "Tap Kartu Anda");
  setLeds(true, false);

  // RFID hanya dibaca saat state LOCKED
  String uid;
  if (readCardUid(uid)) processCard(uid);
}

// ===================== SETUP & LOOP =====================
void setup() {
  Serial.begin(115200);

  pinMode(PIR_PIN, INPUT);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_RED, OUTPUT);

  Wire.begin(SDA_PIN, SCL_PIN);
  lcd.init();

  SPI.begin(RFID_SCK, RFID_MISO, RFID_MOSI, RFID_SS);
  mfrc522.PCD_Init();

  Serial.println("=== Smart Automated Attendance System ===");
  Serial.println("CSV_HEADER,uid,nama,tanggal,jam_scan");
  enterStandby();
}

void loop() {
  unsigned long now = millis();

  // 1) STANDBY: hanya PIR yang dipantau
  if (state == STANDBY) {
    if (digitalRead(PIR_PIN) == HIGH) wakeUp();
    return;
  }

  // 2) Ukur jarak berkala, perbarui waktu terakhir ada orang
  if (now - lastMeasure >= MEASURE_INTERVAL_MS) {
    lastMeasure = now;
    distanceCm = readDistanceCm();
    bool dekat = (distanceCm > 0 && distanceCm <= PRESENCE_MAX_CM);
    if (dekat || digitalRead(PIR_PIN) == HIGH) lastSeen = now;
  }

  // 3) Tidak ada deteksi selama 5 detik -> kembali tidur
  if (state != TRANSAKSI && (now - lastSeen > IDLE_TIMEOUT_MS)) {
    enterStandby();
    return;
  }

  // 4) Jalankan logika sesuai state
  switch (state) {
    case POSITIONING:
      handlePositioning();
      break;
    case LOCKED:
      handleLocked();
      break;
    case TRANSAKSI:
      if (now >= resultUntil) {
        state = POSITIONING;
        cache1 = "";
        cache2 = "";
        Serial.println("[STATE] Selesai transaksi -> POSITIONING");
      }
      break;
    default:
      break;
  }
}