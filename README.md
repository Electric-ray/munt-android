# Munt for Android — MT-32 MIDI over RTP/USB

DOS PC의 MT-32 MIDI 출력을 Android 기기에서 실시간으로 에뮬레이션하는 앱입니다.  
[munt/mt32emu](https://github.com/munt/munt) 라이브러리를 Android에 포팅하였으며,  
ESP32를 경유한 RTP-MIDI(Wi-Fi) 또는 USB-MIDI 두 가지 연결 방식을 지원합니다.








---

## 🎮 시스템 구성

```
DOS PC ──RS232──► ESP32 ──WiFi RTP-MIDI──► Android (Munt for Android)
           └─────────USB OTG────────────►              │
                                                       ▼
                                              mt32emu 오디오 출력
```

| 항목 | 사양 |
|------|------|
| 테스트 기기 | LG Velvet (LG-G910N), Android 10 (API 29) |
| 아키텍처 | arm64-v8a, armeabi-v7a |
| 최소 안드로이드 | Android 10 (API 29) |
| MT-32 ROM | 별도 준비 필요 (저작권으로 미포함) |

---

## 📦 구성 파일

```
munt-android/
├── android-app\                  ← Android 프로젝트 소스
│   ├── app\
│   │   ├── src\main\
│   │   │   ├── cpp\
│   │   │   │   ├── MuntBridge.cpp
│   │   │   │   ├── CMakeLists.txt
│   │   │   │   └── mt32emu\     (56개 파일 — munt 라이브러리)
│   │   │   ├── java\            (5개 Kotlin 파일)
│   │   │   ├── res\             (레이아웃/아이콘)
│   │   │   ├── assets\munt_roms\README.txt  ← ROM 직접 넣는 곳
│   │   │   └── AndroidManifest.xml
│   │   ├── build.gradle.kts
│   │   └── proguard-rules.pro
│   ├── gradle\wrapper\
│   ├── build.gradle.kts
│   ├── settings.gradle.kts
│   ├── gradle.properties
│   ├── local.properties.template  ← SDK 경로 템플릿
│   ├── gradlew / gradlew.bat
├── esp32-firmware/
│   └── dos_midi_v4.ino           ← ESP32 펌웨어 (Arduino IDE용)
└── README.md
```

---

## 🚀 설치 방법

### 1. ESP32 펌웨어

**준비물:** Arduino IDE, ESP32 보드 패키지, lathoub/Arduino-AppleMIDI-Library
(제 다른 저장소의 comMIDI를 안드로이드에 맞춰서 패치했습니다)

```
Arduino IDE → 파일 → 열기 → dos_midi_v4.ino
도구 → 보드 → ESP32S3 Dev Module (또는 사용 중인 ESP32 보드)
업로드
```

**ESP32 AP 설정 (펌웨어 내 기본값):**
- SSID: `DOS_MIDI`
- PW: `12345678`
- IP: `192.168.4.1`
- 포트: 5004/5005 (RTP-MIDI)
- RS-232: GPIO 44, 38400 baud

### 2. Android 앱 설치

```bash
# USB 디버깅 활성화 후:
adb install -r munt-android-v1.0.apk
```

또는 APK 파일을 기기로 복사 후 직접 설치.

### 3. MT-32 ROM 파일 준비

MT-32 ROM은 저작권 문제로 미포함입니다. 별도로 준비하여 아래 경로에 배치:

```
/sdcard/Download/munt_roms/MT32_CONTROL.ROM  (64KB)
/sdcard/Download/munt_roms/MT32_PCM.ROM      (512KB)
```
혹은 android-app\app\src\main\assets\munt_roms\ 에 넣고 처음부터 함께 빌드하셔도 됩니다.

---

## 📡 연결 방법

### RTP-MIDI (Wi-Fi)

1. Android를 ESP32 AP(`DOS_MIDI`)에 연결
2. 앱에서 **[RTP 연결]** 버튼 클릭
3. 연결 완료 후 DOS 게임/음악 프로그램 실행

> **팁:** 반드시 **앱 연결 먼저 → 게임 시작** 순서로 하세요.  
> 게임 시작 후 연결하면 MT-32 초기화 MIDI 신호가 누락될 수 있습니다.

### USB (OTG)

1. ESP32를 USB OTG 케이블로 Android에 연결
2. 앱에서 **[USB 연결]** 버튼 클릭

---

## 🔧 주요 기능

- **RTP-MIDI (AppleMIDI)**: Tobias Erichsen rtpMIDI, lathoub Arduino-AppleMIDI-Library 호환
- **USB-MIDI**: mik3y usb-serial-for-android (v3.7.3)
- **mt32emu 폴리포니**: 32파셜(하드웨어 기본) → **64파셜**로 확장 (음 누락 방지)
- **AAudio 저지연**: EXCLUSIVE 모드, 960fr(30ms) 콜백, 160fr(5ms) MIDI 슬라이스
- **MT-32 상태 복구**: 재연결 시 SysEx 캐시 자동 재생
- **stuck note 방지**: 30초 무신호 시 All Notes Off 자동 전송
- **진단 로깅**: Polyphony overflow 실시간 감지

---

## ⚙️ 설정

### ESP32 펌웨어 설정 변경

`dos_midi_v4.ino` 상단에서 변경:

```cpp
static const char* kSSID   = "DOS_MIDI";   // Wi-Fi SSID
static const char* kPASS   = "12345678";   // Wi-Fi 비밀번호
#define RS232_RX_GPIO 44                    // RS232 RX 핀
#define RS232_BAUD    38400                 // 통신 속도
```

### PC rtpMIDI 연결 (Windows)

Tobias Erichsen [rtpMIDI](https://www.tobias-erichsen.de/software/rtpmidi.html) 설치 후:
- 세션 추가 → `192.168.4.1:5004` 연결

---

## 🐛 알려진 제한사항

| 항목 | 설명 |
|------|------|
| UDP 패킷 손실 | WiFi 특성상 ~1% 패킷 손실 발생 가능. 고음 일부 누락 가능 |
| 악기 배치 | 게임 시작 후 연결 시 초기화 SysEx 누락으로 음색 틀릴 수 있음 |
| GM/GS 미지원 | mt32emu는 MT-32 전용. GS 콘텐츠는 재생은 되나 음색 다름 |

---

## 🛠️ 빌드 환경

- **Android Studio** Hedgehog 이상
- **NDK** r25c 이상
- **CMake** 3.22.1
- **Target SDK** 36 (Android 15)
- **C++ Standard** C++11

```bash
set JAVA_HOME=C:\Program Files\Android\Android Studio\jbr
gradlew.bat assembleRelease
```

---

## 📝 변경 이력

### v1.0 (2026-06-07)
- 최초 릴리즈
- RTP-MIDI CK 패킷 형식 수정 (36바이트 고정, 올바른 SSRC)
- AAudio 저지연 오디오 (EXCLUSIVE, SCHED_FIFO, 슬라이스 기반 MIDI 스케줄링)
- mt32emu 폴리포니 64파셜로 확장 (음 누락 대폭 감소)
- ESP32 noRecentActivity 가드 복원 (핸드쉐이크 안정성)
- MT-32 SysEx 캐시 및 재연결 자동 복구
- ESP32 SysEx uint16_t 수정 (256바이트 초과 SysEx 정상 전송)

---

## 📄 라이선스

- 앱 코드: MIT License
- mt32emu: [LGPLv2.1](https://github.com/munt/munt/blob/master/mt32emu/COPYING.LGPLv2.1)
- usb-serial-for-android: LGPL
- MT-32 ROM: 별도 라이선스 (Rolland Corp.) — 미포함

---

## 🙏 감사

- [munt/munt](https://github.com/munt/munt) — mt32emu 라이브러리
- [lathoub/Arduino-AppleMIDI-Library](https://github.com/lathoub/Arduino-AppleMIDI-Library) — ESP32 RTP-MIDI
- [mik3y/usb-serial-for-android](https://github.com/mik3y/usb-serial-for-android) — USB 시리얼
- [dwhinham/mt32-pi](https://github.com/dwhinham/mt32-pi) — AppleMIDI 프로토콜 참조

## 이 프로젝트는 AI를 이용하여 진행되었습니다.
