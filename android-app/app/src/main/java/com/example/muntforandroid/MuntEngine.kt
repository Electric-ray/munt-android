package com.example.muntforandroid

import android.content.Context
import android.net.*
import android.net.wifi.WifiManager
import android.os.Build
import android.util.Log
import java.io.File

class MuntEngine(val ctx: Context) {

    companion object {
        private const val TAG = "MuntEngine"
        private const val CTRL_ROM = "MT32_CONTROL.ROM"
        private const val PCM_ROM  = "MT32_PCM.ROM"
        init { System.loadLibrary("muntforandroid") }
    }

    external fun init(ctrlRom: ByteArray, pcmRom: ByteArray): Boolean
    external fun sendMidi(packed: Int)
    external fun sendSysEx(data: ByteArray, len: Int)
    external fun resetSynth()
    external fun getStats(): String
    external fun destroy()

    private var rtpSession: RtpMidiSession? = null
    private var usbManager: UsbMidiManager? = null
    private var wifiLock: WifiManager.WifiLock? = null
    var onStatus: ((String) -> Unit)? = null

    // ── ROM 로드: Download/munt_roms/ → assets/munt_roms/ ─────────────
    fun loadRomBytes(filename: String): ByteArray? {
        // 1순위: /sdcard/Download/munt_roms/
        try {
            val dl = File(
                android.os.Environment.getExternalStoragePublicDirectory(
                    android.os.Environment.DIRECTORY_DOWNLOADS),
                "munt_roms/$filename")
            if (dl.exists()) {
                Log.i(TAG, "ROM (Downloads): ${dl.absolutePath}")
                return dl.readBytes()
            }
        } catch (e: Exception) {
            Log.w(TAG, "Downloads 접근 실패: $e")
        }
        // 2순위: assets/munt_roms/
        return try {
            ctx.assets.open("munt_roms/$filename").readBytes().also {
                Log.i(TAG, "ROM (assets): $filename")
            }
        } catch (_: Exception) { null }
    }

    // ── Synth 초기화 ─────────────────────────────────────────────────────
    fun initSynth(): Boolean {
        val ctrl = loadRomBytes(CTRL_ROM) ?: run {
            onStatus?.invoke("❌ $CTRL_ROM 없음\n/sdcard/Download/munt_roms/ 에 넣어주세요")
            return false
        }
        val pcm = loadRomBytes(PCM_ROM) ?: run {
            onStatus?.invoke("❌ $PCM_ROM 없음")
            return false
        }
        onStatus?.invoke("ROM 로드: ctrl=${ctrl.size}B pcm=${pcm.size}B")
        return init(ctrl, pcm).also { ok ->
            if (!ok) onStatus?.invoke("❌ munt init 실패")
        }
    }

    // ── RTP-MIDI (서버 모드) ──────────────────────────────────────────────
    @Volatile var activeWifiNetwork: Network? = null

    fun startRtp() {
        stopUsb()
        val wm = ctx.getSystemService(Context.WIFI_SERVICE) as WifiManager
        @Suppress("DEPRECATION")
        wifiLock = wm.createWifiLock(WifiManager.WIFI_MODE_FULL_HIGH_PERF, "munt_wifi")
        wifiLock?.acquire()

        val cm = ctx.getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
        var launched = false

        cm.requestNetwork(
            NetworkRequest.Builder()
                .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
                // ★ 핵심 수정 1: 인터넷 불필요 명시 → Android가 LTE로 우회하지 않음
                // ESP32 AP는 인터넷 없음 → 기본 설정 시 Android가 LTE로 패킷 라우팅
                // CK1 등 송신 패킷이 LTE로 나가면 ESP32가 받지 못함 → 50~60초 후 BY
                .removeCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
                .build(),
            object : ConnectivityManager.NetworkCallback() {
                override fun onAvailable(net: Network) {
                    activeWifiNetwork = net
                    cm.bindProcessToNetwork(net)
                    rtpSession?.updateNetwork(net)
                    onStatus?.invoke("WiFi 바인딩 완료")
                    if (!launched) { launched = true; launchRtp(net) }
                }
                override fun onLost(net: Network) {
                    activeWifiNetwork = null
                    rtpSession?.updateNetwork(null)
                    onStatus?.invoke("⚠️ WiFi 재확인 중 (세션 유지)")
                }
            })
    }

    private fun launchRtp(net: Network?) {
        replaySysexCache()
        rtpSession = RtpMidiSession(
            ctx = ctx,
            onMidiMessage = ::dispatchMidi,
            onStatus = { msg -> onStatus?.invoke(msg) },
            onAllNotesOff = ::allNotesOff
        ).also { it.start(net) }
    }

    fun stopRtp() {
        rtpSession?.stop(); rtpSession = null
        wifiLock?.release(); wifiLock = null
    }

    // ── USB ──────────────────────────────────────────────────────────────
    fun startUsb(): Boolean {
        stopRtp()
        return UsbMidiManager(ctx, ::dispatchMidi).also { m ->
            m.onStatus = { msg -> onStatus?.invoke(msg) }
            usbManager = m
        }.connect()
    }

    fun stopUsb() { usbManager?.disconnect(); usbManager = null }

    // ── MT-32 SysEx 캐시 (재연결 후 악기 복구) ───────────────────────────
    // 문제: RTP 재연결 시 게임이 MT-32 초기화 SysEx를 재전송하지 않음
    //       → 재연결 후 mt32emu가 기본 상태 → 악기 배치 틀어짐
    // 해결: model=0x16(MT-32) SysEx를 캐시 → 재연결 시 mt32emu에 재생
    private val mt32SysexCache = mutableListOf<ByteArray>()
    private val mt32CacheLock  = Any()

    // ── MIDI 디스패치 ─────────────────────────────────────────────────────
    fun dispatchMidi(bytes: ByteArray) {
        if (bytes.isEmpty()) return
        if (bytes[0] == 0xF0.toByte()) {
            // MT-32 SysEx(model=0x16) 캐싱 — 재연결 복구용
            if (bytes.size >= 4 && (bytes[3].toInt() and 0xFF) == 0x16) {
                synchronized(mt32CacheLock) { mt32SysexCache.add(bytes.copyOf()) }
                Log.d(TAG, "MT-32 SysEx 캐시: ${bytes.size}B (총 ${mt32SysexCache.size}개)")
            }
            sendSysEx(bytes, bytes.size)
        } else {
            val st = bytes.getOrElse(0){0}.toInt() and 0xFF
            val d1 = bytes.getOrElse(1){0}.toInt() and 0xFF
            val d2 = bytes.getOrElse(2){0}.toInt() and 0xFF
            sendMidi(st or (d1 shl 8) or (d2 shl 16))
        }
    }

    // ── 리셋: JNI All Notes Off + MT-32 Master Reset + SysEx 캐시 재생 ──────
    fun fullReset() {
        resetSynth()
        replaySysexCache()
        onStatus?.invoke("🔄 MT-32 리셋 완료 — 게임 음악을 재시작하세요")
    }

    // stuck note 방지: NoteOff UDP 손실 시 음이 끊기지 않는 현상 해소
    fun allNotesOff() {
        for (ch in 0..8) {
            sendMidi((0xB0 or ch) or (123 shl 8))  // All Notes Off
            sendMidi((0xB0 or ch) or (120 shl 8))  // All Sound Off
        }
    }

    // 캐시된 MT-32 SysEx를 mt32emu에 재생 (재연결 시 호출)
    private fun replaySysexCache() {
        val cache = synchronized(mt32CacheLock) { mt32SysexCache.toList() }
        if (cache.isEmpty()) return
        Log.i(TAG, "MT-32 SysEx 재생: ${cache.size}개")
        onStatus?.invoke("🔄 MT-32 상태 복구 중 (${cache.size}개 SysEx 재생)")
        cache.forEach { sendSysEx(it, it.size) }
    }

    fun stop() { stopRtp(); stopUsb(); destroy() }
}
