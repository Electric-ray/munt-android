/**
 * MuntBridge.cpp v3 — JNI: mt32emu + AAudio
 *
 * [버그수정] MIDI 이벤트 큐 도입:
 *   - sendMidi/sendSysEx: audio mutex 없이 g_evMtx 큐에 push (즉시 반환)
 *   - aaCallback: render 전에 큐 전체 drain → Note On+Off 타이밍 정상화
 *   - 이유: RTP 패킷 1개에 NoteOn+NoteOff 묶여 올 때,
 *     sendMidi가 audio mutex에 60ms 블로킹되어 두 메시지가
 *     같은 오디오 프레임 내에 처리 → 0ms 발음 → 무음
 */
#include <jni.h>
#include <android/log.h>
#include <aaudio/AAudio.h>
#include <cstring>
#include <cstdint>
#include <mutex>
#include <atomic>
#include <deque>
#include <vector>
#include <memory>
#include <sched.h>

#include "mt32emu.h"

#define TAG  "MuntBridge"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// ── 진단용 ReportHandler ─────────────────────────────────────────────────
// mt32emu에서 오는 경고/이벤트를 logcat으로 출력
// 음 누락/볼륨 감소 원인 파악:
//   onNoteOnIgnored      → 폴리포니 한계(32파셜)로 NoteOn 무시됨 ← 음 누락 원인
//   onPlayingPolySilenced → 재생 중 음이 강제로 잘림 ← 볼륨 갑자기 작아지는 원인
class DiagReportHandler : public MT32Emu::ReportHandler3 {
public:
    void onNoteOnIgnored(MT32Emu::Bit32u partialsNeeded, MT32Emu::Bit32u partialsFree) override {
        // ★ 이 로그가 뜨면 = 폴리포니 오버플로우 = 음 누락 원인 확정
        __android_log_print(ANDROID_LOG_WARN, "MuntBridge",
            "⚠️ NoteOn IGNORED: 필요=%u 남은파셜=%u (32파셜 한계 초과)",
            partialsNeeded, partialsFree);
    }
    void onPlayingPolySilenced(MT32Emu::Bit32u partialsNeeded, MT32Emu::Bit32u partialsFree) override {
        // ★ 이 로그가 뜨면 = 재생 중인 음을 강제로 끊음 = 볼륨 감소 원인 확정
        __android_log_print(ANDROID_LOG_WARN, "MuntBridge",
            "⚠️ Poly SILENCED: 필요=%u 남은파셜=%u (재생 중 음 강제종료)",
            partialsNeeded, partialsFree);
    }
    void onPolyStateChanged(MT32Emu::Bit8u partNum) override {
        // 파트별 폴리 상태 변화 (너무 빈번하면 주석처리)
        // LOGI("PolyChange: part=%d", partNum);
    }
    void onProgramChanged(MT32Emu::Bit8u partNum, const char* groupName, const char* patchName) override {
        LOGI("ProgramChange: ch%d → %s / %s", partNum+1, groupName, patchName);
    }
    void showLCDMessage(const char* message) override {
        LOGI("MT-32 LCD: %s", message);
    }
    void printDebug(const char* fmt, va_list list) override {
        char buf[256];
        vsnprintf(buf, sizeof(buf), fmt, list);
        __android_log_print(ANDROID_LOG_DEBUG, "MuntBridge", "mt32emu: %s", buf);
    }
};
static DiagReportHandler g_reportHandler;

// ── ROM / Synth ───────────────────────────────────────────────────────────
static uint8_t* g_ctrlData = nullptr; static size_t g_ctrlLen = 0;
static uint8_t* g_pcmData  = nullptr; static size_t g_pcmLen  = 0;
static const MT32Emu::ROMImage* g_ctrlImg = nullptr;
static const MT32Emu::ROMImage* g_pcmImg  = nullptr;
static MT32Emu::Synth* g_synth = nullptr;
static std::mutex       g_mtx;   // synth render 전용

// ── MIDI 이벤트 큐 ────────────────────────────────────────────────────────
// sendMidi/sendSysEx → g_evQ (non-blocking)
// aaCallback         → drain g_evQ → render
struct MidiEv {
    bool isSysex;
    uint32_t msg;                          // 일반 MIDI
    std::shared_ptr<std::vector<uint8_t>> sysex; // SysEx
};
static std::deque<MidiEv> g_evQ;
static std::mutex         g_evMtx;        // 큐 전용 (audio mutex와 분리)

// ── 통계 ─────────────────────────────────────────────────────────────────
static std::atomic<uint64_t> g_midiCount{0};
static std::atomic<uint64_t> g_sysexCount{0};
static std::atomic<int>      g_logCount{0};

// ── AAudio ────────────────────────────────────────────────────────────────
#define SAMPLE_RATE 32000
static AAudioStream* g_aaStream = nullptr;
static bool g_threadPrioSet = false;

// ── 슬라이스 크기: 160fr = 5ms @32kHz ────────────────────────────────────
// 콜백 1920fr(60ms)을 이 크기로 쪼개어 각 슬라이스 시작마다 큐 drain
// → MIDI 이벤트 도착 후 최대 5ms 내에 반영 (이전: 최대 60ms)
// → Kotlin 스레드가 g_evMtx만 잡고 push하므로 슬라이스 사이에도 수신 가능
#define MIDI_SLICE_FRAMES 160

static aaudio_data_callback_result_t aaCallback(
        AAudioStream*, void*, void* audioData, int32_t numFrames) {

    if (!g_threadPrioSet) {
        g_threadPrioSet = true;
        sched_param sp{}; sp.sched_priority = sched_get_priority_max(SCHED_FIFO);
        pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    }

    auto* buf = static_cast<int16_t*>(audioData);
    std::lock_guard<std::mutex> lk(g_mtx);  // synth 보호 (전체 콜백)

    if (!g_synth) {
        memset(buf, 0, numFrames * 2 * sizeof(int16_t));
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }

    // ★ 슬라이스 기반 MIDI 스케줄링
    // [drain → render 160fr] × N 반복
    // g_evMtx는 슬라이스마다 짧게 잡고 해제 → Kotlin push와 최소 경합
    int32_t offset = 0;
    while (offset < numFrames) {
        // 슬라이스 시작: 큐 drain
        {
            std::lock_guard<std::mutex> qlk(g_evMtx);
            while (!g_evQ.empty()) {
                auto& ev = g_evQ.front();
                if (ev.isSysex && ev.sysex)
                    g_synth->playSysex(
                        (const MT32Emu::Bit8u*)ev.sysex->data(),
                        (MT32Emu::Bit32u)ev.sysex->size());
                else
                    g_synth->playMsg((MT32Emu::Bit32u)ev.msg);
                g_evQ.pop_front();
            }
        }
        // 슬라이스 렌더 (남은 프레임이 MIDI_SLICE_FRAMES보다 작으면 나머지만)
        int32_t toRender = std::min(MIDI_SLICE_FRAMES, numFrames - offset);
        g_synth->render(buf + offset * 2, (MT32Emu::Bit32u)toRender);
        offset += toRender;
    }

    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static bool startAAudio() {
    // 콜백: 960fr(30ms) — 이전 1920fr(60ms)에서 절반으로 줄임
    // 이유: 경계마다 최대 60ms 갭 → 30ms로 완화
    // 640fr 이하 지지직 확인, 960fr = 6 bursts @48kHz → 안정 예상
    constexpr int32_t CB_FRAMES  = 960;
    constexpr int32_t BUF_FRAMES = CB_FRAMES * 6;  // 180ms 버퍼

    AAudioStreamBuilder* builder = nullptr;
    if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK) {
        LOGE("builder 생성 실패"); return false;
    }
    AAudioStreamBuilder_setDirection      (builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSampleRate     (builder, SAMPLE_RATE);
    AAudioStreamBuilder_setChannelCount   (builder, 2);
    AAudioStreamBuilder_setFormat         (builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode    (builder, AAUDIO_SHARING_MODE_EXCLUSIVE);
    AAudioStreamBuilder_setDataCallback   (builder, aaCallback, nullptr);
    AAudioStreamBuilder_setFramesPerDataCallback(builder, CB_FRAMES);
    AAudioStreamBuilder_setBufferCapacityInFrames(builder, BUF_FRAMES);

    aaudio_result_t r = AAudioStreamBuilder_openStream(builder, &g_aaStream);
    if (r != AAUDIO_OK) {
        LOGE("EXCLUSIVE 실패(%d), SHARED 재시도", r);
        AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
        r = AAudioStreamBuilder_openStream(builder, &g_aaStream);
    }
    AAudioStreamBuilder_delete(builder);
    if (r != AAUDIO_OK) { LOGE("스트림 열기 실패: %d", r); return false; }

    int32_t rate  = AAudioStream_getSampleRate(g_aaStream);
    int32_t burst = AAudioStream_getFramesPerBurst(g_aaStream);
    int32_t cap   = AAudioStream_getBufferCapacityInFrames(g_aaStream);
    LOGI("AAudio: %dHz cb=%dfr(%.1fms) slice=%dfr(%.1fms) cap=%dfr burst=%dfr sharing=%d",
         rate, CB_FRAMES, CB_FRAMES*1000.0/SAMPLE_RATE,
         MIDI_SLICE_FRAMES, MIDI_SLICE_FRAMES*1000.0/SAMPLE_RATE,
         cap, burst, (int)AAudioStream_getSharingMode(g_aaStream));
    if (rate != SAMPLE_RATE)
        LOGI("⚠️ %dHz→%dHz OS 리샘플 중", SAMPLE_RATE, rate);

    r = AAudioStream_requestStart(g_aaStream);
    if (r != AAUDIO_OK) { LOGE("start 실패: %d", r); return false; }
    return true;
}

static void stopAAudio() {
    if (g_aaStream) {
        AAudioStream_requestStop(g_aaStream);
        AAudioStream_close(g_aaStream);
        g_aaStream = nullptr;
    }
    g_threadPrioSet = false;
}

static void freeRom() {
    if (g_ctrlImg){MT32Emu::ROMImage::freeROMImage(g_ctrlImg);g_ctrlImg=nullptr;}
    if (g_pcmImg) {MT32Emu::ROMImage::freeROMImage(g_pcmImg); g_pcmImg=nullptr;}
    delete[] g_ctrlData; g_ctrlData=nullptr; g_ctrlLen=0;
    delete[] g_pcmData;  g_pcmData=nullptr;  g_pcmLen=0;
}

extern "C" {

// ── init ──────────────────────────────────────────────────────────────────
JNIEXPORT jboolean JNICALL
Java_com_example_muntforandroid_MuntEngine_init(
        JNIEnv* env, jobject, jbyteArray ctrlRom, jbyteArray pcmRom) {
    stopAAudio();
    { std::lock_guard<std::mutex> lk(g_mtx);
      if (g_synth) { g_synth->close(); delete g_synth; g_synth=nullptr; } }
    { std::lock_guard<std::mutex> qlk(g_evMtx); g_evQ.clear(); }
    freeRom();
    g_midiCount=0; g_sysexCount=0; g_logCount=0;

    jsize cLen=env->GetArrayLength(ctrlRom), pLen=env->GetArrayLength(pcmRom);
    jbyte *cTmp=env->GetByteArrayElements(ctrlRom,nullptr),
          *pTmp=env->GetByteArrayElements(pcmRom,nullptr);
    g_ctrlData=new uint8_t[(size_t)cLen]; memcpy(g_ctrlData,cTmp,(size_t)cLen); g_ctrlLen=(size_t)cLen;
    g_pcmData =new uint8_t[(size_t)pLen]; memcpy(g_pcmData, pTmp,(size_t)pLen); g_pcmLen=(size_t)pLen;
    env->ReleaseByteArrayElements(ctrlRom,cTmp,JNI_ABORT);
    env->ReleaseByteArrayElements(pcmRom, pTmp,JNI_ABORT);

    MT32Emu::ArrayFile cf((const MT32Emu::Bit8u*)g_ctrlData,g_ctrlLen);
    MT32Emu::ArrayFile pf((const MT32Emu::Bit8u*)g_pcmData, g_pcmLen);
    g_ctrlImg=MT32Emu::ROMImage::makeROMImage(&cf);
    g_pcmImg =MT32Emu::ROMImage::makeROMImage(&pf);
    if (!g_ctrlImg||!g_pcmImg) { LOGE("ROM 실패"); freeRom(); return JNI_FALSE; }

    const MT32Emu::ROMInfo* ci=g_ctrlImg->getROMInfo();
    const MT32Emu::ROMInfo* pi=g_pcmImg->getROMInfo();
    LOGI("ROM ctrl=%s pcm=%s", ci?ci->shortName:"?", pi?pi->shortName:"?");

    { std::lock_guard<std::mutex> lk(g_mtx);
      g_synth = new MT32Emu::Synth(&g_reportHandler);
      g_synth->setReportHandler3(&g_reportHandler);
      // ★ 파셜 수 32→64: 폴리포니 오버플로우(음 누락/강제종료) 해결
      // MT-32 하드웨어는 32파셜 고정이지만 mt32emu는 소프트웨어로 확장 가능
      // 복잡한 곡에서 SILENCED/IGNORED가 빈번 → 64로 늘려 voice stealing 최소화
      // CPU 영향: Snapdragon 765G 기준 미미 (파셜당 약 1kHz 계산 추가)
      if (!g_synth->open(*g_ctrlImg, *g_pcmImg, 64)) {
          LOGE("Synth::open 실패"); delete g_synth; g_synth=nullptr; freeRom(); return JNI_FALSE; } }

    if (!startAAudio()) {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_synth->close(); delete g_synth; g_synth=nullptr; freeRom(); return JNI_FALSE; }

    LOGI("init OK — MIDI 큐 방식 활성화");
    return JNI_TRUE;
}

// ── sendMidi: 큐에 push (non-blocking) ───────────────────────────────────
JNIEXPORT void JNICALL
Java_com_example_muntforandroid_MuntEngine_sendMidi(JNIEnv*,jobject,jint packed) {
    uint32_t u = (uint32_t)packed;
    ++g_midiCount;

    int cnt = g_logCount.fetch_add(1);
    if (cnt < 50) {
        LOGI("MIDI[%d]: 0x%06X (st=0x%02X d1=%d d2=%d)",
             cnt, u & 0xFFFFFF,
             u & 0xFF, (u>>8)&0xFF, (u>>16)&0xFF);
    }

    std::lock_guard<std::mutex> qlk(g_evMtx);
    g_evQ.push_back({false, u, nullptr});
}

// ── sendSysEx: 큐에 push (non-blocking) ──────────────────────────────────
JNIEXPORT void JNICALL
Java_com_example_muntforandroid_MuntEngine_sendSysEx(JNIEnv* env,jobject,jbyteArray data,jint len) {
    ++g_sysexCount;
    // SysEx 앞 10바이트 로그
    jbyte* buf=env->GetByteArrayElements(data,nullptr);
    char hex[64]={0};
    int show = (len < 10) ? len : 10;
    for(int i=0;i<show;i++) sprintf(hex+i*3,"%02X ",(uint8_t)buf[i]);
    LOGI("SysEx[%llu]: %d bytes [%s...]",
         (unsigned long long)g_sysexCount.load(), len, hex);

    auto sysexVec = std::make_shared<std::vector<uint8_t>>(
        (uint8_t*)buf, (uint8_t*)buf + len);
    env->ReleaseByteArrayElements(data, buf, JNI_ABORT);

    std::lock_guard<std::mutex> qlk(g_evMtx);
    g_evQ.push_back({true, 0, sysexVec});
}

// ── resetSynth ────────────────────────────────────────────────────────────
// 1단계: 큐 클리어 + All Notes Off (stuck note 방지)
// 2단계: MT-32 Master Reset SysEx → mt32emu ROM 기본 상태로 복원
//   이후 게임이 Program Change를 보내면 올바른 악기로 복원됨
JNIEXPORT void JNICALL
Java_com_example_muntforandroid_MuntEngine_resetSynth(JNIEnv*,jobject) {
    std::lock_guard<std::mutex> qlk(g_evMtx);
    g_evQ.clear();
    if (!g_synth) return;

    // 1단계: All Notes Off + All Sound Off
    for (int ch = 0; ch < 10; ch++) {
        if (ch == 8) continue;
        g_evQ.push_back({false, (uint32_t)(0xB0|ch)|(123u<<8)|(0u<<16), nullptr});
        g_evQ.push_back({false, (uint32_t)(0xB0|ch)|(120u<<8)|(0u<<16), nullptr});
    }

    // 2단계: MT-32 Reset SysEx → mt32emu 기본 ROM 패치 상태로 복원
    // F0 41 10 16 12 7F 00 00 01 00 F7  (Roland MT-32 All Parameters Reset)
    static const uint8_t kMT32Reset[] = {
        0xF0, 0x41, 0x10, 0x16, 0x12,
        0x7F, 0x00, 0x00, 0x01, 0x00,
        0xF7
    };
    auto sysex = std::make_shared<std::vector<uint8_t>>(
        kMT32Reset, kMT32Reset + sizeof(kMT32Reset));
    g_evQ.push_back({true, 0, sysex});

    LOGI("resetSynth: All Notes Off + MT-32 Master Reset");
}

// ── getStats ──────────────────────────────────────────────────────────────
JNIEXPORT jstring JNICALL
Java_com_example_muntforandroid_MuntEngine_getStats(JNIEnv* env,jobject) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g_synth) return env->NewStringUTF("partStates:0\nnames:\nmidi:0\nsysex:0\nactive:0\n");
    char buf[1024];
    MT32Emu::Bit32u states=g_synth->getPartStates();
    char names[512]={0};
    for(int i=0;i<9;i++){
        const char* n=g_synth->getPatchName((MT32Emu::Bit8u)i);
        if(i>0) strcat(names,",");
        strncat(names,n?n:"---",20);
    }
    size_t qsz;
    { std::lock_guard<std::mutex> qlk(g_evMtx); qsz=g_evQ.size(); }
    snprintf(buf,sizeof(buf),
        "partStates:%u\nnames:%s\nmidi:%llu\nsysex:%llu\nactive:%d\nqsz:%zu\n",
        (unsigned)states, names,
        (unsigned long long)g_midiCount.load(),
        (unsigned long long)g_sysexCount.load(),
        g_synth->isActive()?1:0, qsz);
    return env->NewStringUTF(buf);
}

// ── destroy ───────────────────────────────────────────────────────────────
JNIEXPORT void JNICALL
Java_com_example_muntforandroid_MuntEngine_destroy(JNIEnv*,jobject) {
    stopAAudio();
    { std::lock_guard<std::mutex> qlk(g_evMtx); g_evQ.clear(); }
    { std::lock_guard<std::mutex> lk(g_mtx);
      if (g_synth) { g_synth->close(); delete g_synth; g_synth=nullptr; } }
    freeRom();
    LOGI("destroy OK");
}

} // extern "C"
