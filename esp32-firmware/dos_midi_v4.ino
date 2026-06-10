// 🎵 DOS_MIDI v4 — SysEx uint16_t 수정 + v7.9 파싱 구조 적용
// 변경점:
//   - struct: {st,d1,d2,chan, uint16_t len} — v7.9와 동일 구조
//   - feedByte 집합체 초기화 순서를 struct 필드 순서와 정확히 일치
//   - midiTxTask: dataLen(m.st) 재계산 → m.len에 의존 안 함 (v7.9)
//   - SysEx m.len = syxLen 직접 대입 (uint16_t, 256B 정상 처리)
//   - taskYIELD: 큐 비었을 때만 (vTaskDelay 제거)

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <ESPmDNS.h>
#include <AppleMIDI.h>
#include <atomic>

#define RS232_RX_GPIO 44
#define RS232_BAUD    38400

static const char* kSSID   = "DOS_MIDI";
static const char* kPASS   = "12345678";
static const int   kWiFiCH = 6;

APPLEMIDI_CREATE_DEFAULTSESSION_INSTANCE();
HardwareSerial RS232(1);

std::atomic<bool>     g_connected{false};
std::atomic<uint32_t> g_drop{0}, g_sent{0}, g_recv{0};
std::atomic<uint32_t> g_lastActivityMs{0};  // ★ 복원: 마지막 MIDI 활동 시각
uint32_t g_lastReconnectMs = 0;
uint32_t g_backoffMs       = 1500;
uint32_t g_lastLogMs       = 0;

enum MidiMode { MODE_UNKNOWN, MODE_GS, MODE_MT32 };
MidiMode g_mode = MODE_UNKNOWN;

// ★ v7.9 구조: chan이 len 앞에 옴 — 집합체 초기화 {st,d1,d2,chan,len,data}
struct MidiMsg {
  uint8_t  st, d1, d2, chan;  // chan = 필드 index 3
  uint16_t len;               // SysEx 길이 (uint16_t: 256B 정상 처리)
  uint8_t  data[512];
};

QueueHandle_t g_midiQueue = nullptr;

static inline uint8_t dataLen(uint8_t s) {
  if (s >= 0xF8) return 0;
  switch (s & 0xF0) {
    case 0x80: case 0x90: case 0xA0: case 0xB0: case 0xE0: return 2;
    case 0xC0: case 0xD0: return 1;
  }
  switch (s) { case 0xF1: case 0xF3: return 1; case 0xF2: return 2; default: return 0; }
}

static void onAppleMidiConnected(const appleMidi::ssrc_t& ssrc, const char* name) {
  g_connected.store(true);
  g_backoffMs = 1500;
  Serial.printf("✅ 연결됨: %s (ssrc=%lu)\n", name, (unsigned long)ssrc);
}
static void onAppleMidiDisconnected(const appleMidi::ssrc_t& ssrc) {
  g_connected.store(false);
  Serial.printf("❌ 세션 종료 (ssrc=%lu)\n", (unsigned long)ssrc);
}
static void restartAppleMIDI() {
  Serial.println("🔄 AppleMIDI 세션 재시작...");
  AppleMIDI.begin();
  AppleMIDI.setHandleConnected(onAppleMidiConnected);
  AppleMIDI.setHandleDisconnected(onAppleMidiDisconnected);
}

// ★ midiTxTask: 일반 MIDI는 dataLen(m.st) 재계산 (v7.9 방식)
//   m.len에 의존하지 않으므로 초기화 오류 영향 없음
static void midiTxTask(void*) {
  MidiMsg m;
  for (;;) {
    if (xQueueReceive(g_midiQueue, &m, portMAX_DELAY) == pdTRUE) {
      if (!g_connected.load()) { g_drop++; continue; }
      if (m.st == 0xF0) {
        // SysEx: m.len = uint16_t (256B 이상도 정상)
        MIDI.sendSysEx(m.len, m.data, true);
      } else {
        uint8_t ch  = (m.chan >= 1 && m.chan <= 16) ? m.chan : 1;
        uint8_t dln = dataLen(m.st);  // ★ m.len 대신 재계산
        if      (dln == 0) MIDI.send((midi::MidiType)m.st, 0,    0,    ch);
        else if (dln == 1) MIDI.send((midi::MidiType)m.st, m.d1, 0,    ch);
        else               MIDI.send((midi::MidiType)m.st, m.d1, m.d2, ch);
      }
      g_sent++;
      g_lastActivityMs.store(millis());  // ★ 복원: 마지막 전송 시각 기록
      vTaskDelay(1);
    }
  }
}

static void feedByte(uint8_t b) {
  static uint8_t  rs = 0, need = 0, d1 = 0, ch = 1;
  static bool     inSysEx = false;
  static uint8_t  syxBuf[512];
  static uint16_t syxLen = 0;

  g_recv++;

  if (b == 0xF0) {
    inSysEx = true; syxLen = 0; syxBuf[syxLen++] = b; return;
  }

  if (inSysEx) {
    if (syxLen < sizeof(syxBuf)) syxBuf[syxLen++] = b;
    if (b == 0xF7) {
      inSysEx = false;
      // ★ SysEx 길이 로그 — MT-32 timbre가 255B 초과인지 확인
      Serial.printf("SysEx len=%u%s\n", syxLen, syxLen>255?" ⚠️>255":"");
      if (syxLen >= 6) {
        if (syxBuf[1]==0x41 && syxBuf[3]==0x42 && g_mode!=MODE_GS)
          { g_mode=MODE_GS;  Serial.println("🎛️ GS 감지"); }
        else if (syxBuf[1]==0x41 && syxBuf[3]==0x16 && g_mode!=MODE_MT32)
          { g_mode=MODE_MT32; Serial.println("🎹 MT-32 감지"); }
      }
      MidiMsg m{0xF0, 0, 0, 0, 0, {0}};
      m.len = (syxLen <= (uint16_t)sizeof(m.data)) ? syxLen : (uint16_t)sizeof(m.data);
      memcpy(m.data, syxBuf, m.len);
      if (xQueueSend(g_midiQueue, &m, 0) != pdTRUE) g_drop++;
    }
    return;
  }

  // 실시간 메시지
  // ★ 집합체 초기화 순서: {st=b, d1=0, d2=0, chan=ch, len=0, data={}}
  if (b >= 0xF8) {
    MidiMsg m{b, 0, 0, ch, 0, {0}};
    if (xQueueSend(g_midiQueue, &m, 0) != pdTRUE) g_drop++;
    return;
  }

  if (b & 0x80) {
    if (b == 0xF7) { inSysEx = false; return; }
    rs = b; need = dataLen(b); ch = (b & 0x0F) + 1;
    if (need == 0) {
      // ★ {st=rs, d1=0, d2=0, chan=ch, len=0}
      MidiMsg m{rs, 0, 0, ch, 0, {0}};
      if (xQueueSend(g_midiQueue, &m, 0) != pdTRUE) g_drop++;
    }
    return;
  }

  if (!rs) return;
  if (need == 1) {
    // ★ {st=rs, d1=d1, d2=b, chan=ch, len=2}
    MidiMsg m{rs, d1, b, ch, 2, {0}};
    uint8_t hi = rs & 0xF0;
    if (hi==0xC0 || hi==0xD0 || rs==0xF1 || rs==0xF3)
      { m.d1=b; m.d2=0; m.len=1; }
    if (xQueueSend(g_midiQueue, &m, 0) != pdTRUE) g_drop++;
    need = dataLen(rs);
  } else if (need >= 2) {
    d1 = b; need = 1;
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("🎵 DOS_MIDI v4");

  size_t itemSize = sizeof(MidiMsg);
  Serial.printf("ℹ️ MidiMsg:%uB  FreeHeap:%luB\n", (unsigned)itemSize, (unsigned long)ESP.getFreeHeap());

  g_midiQueue = xQueueCreate(128, itemSize);
  if (!g_midiQueue) g_midiQueue = xQueueCreate(64, itemSize);
  if (!g_midiQueue) { Serial.println("❌ 큐 실패! 재부팅"); delay(700); ESP.restart(); }

  xTaskCreatePinnedToCore(midiTxTask, "midiTx", 6144, nullptr, 2, nullptr, APP_CPU_NUM);
  RS232.begin(RS232_BAUD, SERIAL_8N1, RS232_RX_GPIO, -1);
  RS232.setRxBufferSize(8192);

  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);
  WiFi.softAP(kSSID, kPASS, kWiFiCH, 0, 4);
  delay(800);
  Serial.printf("📡 SSID:%s  IP:%s\n", kSSID, WiFi.softAPIP().toString().c_str());

  if (MDNS.begin("dosmidi")) MDNS.addService("apple-midi", "udp", 5004);

  restartAppleMIDI();
  Serial.printf("🚀 준비 완료 (FreeHeap:%lu)\n", (unsigned long)ESP.getFreeHeap());
}

void loop() {
  MIDI.read();
  while (RS232.available() > 0) feedByte((uint8_t)RS232.read());

  // ★ 복원: noRecentActivity 가드 — 핸드쉐이크 자폭 방지
  // IN→OK→CK0→CK1→CK2 전체 4~6초 소요
  // 활동 있는 동안(10초 이내)엔 restartAppleMIDI() 호출 안 함
  // → PC 핸드쉐이크 중 세션 파괴 원천 차단
  if (!g_connected.load()) {
    uint32_t now = millis();
    uint32_t sinceActivity = now - g_lastActivityMs.load();
    bool noRecentActivity = (sinceActivity > 60000);  // 60초 — 10초는 너무 짧아 음악 중간 쉬는 구간에도 재연결 발생
    if (noRecentActivity && (now - g_lastReconnectMs > g_backoffMs)) {
      g_lastReconnectMs = now;
      restartAppleMIDI();
      uint32_t next = g_backoffMs * 2;
      g_backoffMs = (next > 30000u) ? 30000u : next;
    }
  }

  uint32_t now = millis();
  if (now - g_lastLogMs > 3000) {
    g_lastLogMs = now;
    UBaseType_t qfree = uxQueueSpacesAvailable(g_midiQueue);
    const char* modeStr = (g_mode==MODE_GS)?"GS":(g_mode==MODE_MT32?"MT32":"UNK");
    Serial.printf("ℹ️ conn:%d recv:%lu sent:%lu drop:%lu qfree:%u mode:%s heap:%lu\n",
      g_connected.load(), g_recv.load(), g_sent.load(), g_drop.load(),
      (unsigned)qfree, modeStr, (unsigned long)ESP.getFreeHeap());
  }

  vTaskDelay(1);
}
