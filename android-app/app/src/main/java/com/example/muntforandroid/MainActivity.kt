package com.example.muntforandroid

import android.Manifest
import android.content.pm.PackageManager
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.View
import android.widget.*
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat

class MainActivity : AppCompatActivity() {

    companion object {
        private const val REQ_STORAGE = 1001
        private const val STATS_MS    = 120L
        private const val LED_OFF     = 0xFF333355.toInt()
        private const val LED_ON      = 0xFF00EE44.toInt()
        private const val LED_RHY_OFF = 0xFF332222.toInt()
        private const val LED_RHY_ON  = 0xFFEE2222.toInt()
    }

    private lateinit var engine:   MuntEngine
    private lateinit var btnRtp:   Button
    private lateinit var btnUsb:   Button
    private lateinit var btnReset: Button
    private lateinit var tvCounter: TextView
    private lateinit var tvLog:    TextView
    private lateinit var svLog:    ScrollView

    private val leds    = arrayOfNulls<View>(9)
    private val patches = arrayOfNulls<TextView>(9)

    private var rtpOn = false; private var usbOn = false; private var synthReady = false
    private var pendingAction: (() -> Unit)? = null

    private val handler = Handler(Looper.getMainLooper())
    private val statsRunnable = object : Runnable {
        override fun run() {
            if (synthReady) updateStats()
            handler.postDelayed(this, STATS_MS)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        engine    = MuntEngine(applicationContext)
        btnRtp    = findViewById(R.id.btnRtp)
        btnUsb    = findViewById(R.id.btnUsb)
        btnReset  = findViewById(R.id.btnReset)
        tvCounter = findViewById(R.id.tvCounter)
        tvLog     = findViewById(R.id.tvLog)
        svLog     = findViewById(R.id.svLog)

        val ledIds   = intArrayOf(R.id.led0,R.id.led1,R.id.led2,R.id.led3,
                                  R.id.led4,R.id.led5,R.id.led6,R.id.led7,R.id.led8)
        val patchIds = intArrayOf(R.id.tvPatch0,R.id.tvPatch1,R.id.tvPatch2,R.id.tvPatch3,
                                  R.id.tvPatch4,R.id.tvPatch5,R.id.tvPatch6,R.id.tvPatch7,R.id.tvPatch8)
        for (i in 0..8) { leds[i] = findViewById(ledIds[i]); patches[i] = findViewById(patchIds[i]) }

        engine.onStatus = { msg -> runOnUiThread { log(msg) } }

        btnRtp.setOnClickListener {
            if (!synthReady) checkAndInit { doRtp() } else doRtp()
        }
        btnUsb.setOnClickListener {
            if (!synthReady) checkAndInit { doUsb() } else doUsb()
        }
        btnReset.setOnClickListener {
            if (synthReady) {
                engine.fullReset()
                log("🔄 MT-32 리셋 — 게임 음악을 재시작하세요")
            }
        }

        if (intent?.action == "android.hardware.usb.action.USB_DEVICE_ATTACHED")
            checkAndInit { doUsb() }
    }

    private fun doRtp() {
        if (rtpOn) {
            engine.stopRtp(); rtpOn = false; btnRtp.text = "RTP"
            log("RTP 서버 중지")
        } else {
            if (usbOn) { engine.stopUsb(); usbOn = false; btnUsb.text = "USB" }
            engine.startRtp(); rtpOn = true; btnRtp.text = "RTP ●"
            log("RTP 서버 시작 — DOS_MIDI AP에 연결 후 ESP32가 자동 접속")
        }
    }

    private fun doUsb() {
        if (usbOn) { engine.stopUsb(); usbOn = false; btnUsb.text = "USB" }
        else {
            if (rtpOn) { engine.stopRtp(); rtpOn = false; btnRtp.text = "RTP" }
            usbOn = engine.startUsb(); btnUsb.text = if (usbOn) "USB ●" else "USB"
        }
    }

    private fun checkAndInit(onReady: () -> Unit) {
        if (synthReady) { onReady(); return }
        pendingAction = onReady
        if (ContextCompat.checkSelfPermission(this, Manifest.permission.READ_EXTERNAL_STORAGE)
            != PackageManager.PERMISSION_GRANTED) {
            ActivityCompat.requestPermissions(this,
                arrayOf(Manifest.permission.READ_EXTERNAL_STORAGE), REQ_STORAGE)
        } else runInit(onReady)
    }

    override fun onRequestPermissionsResult(rc: Int, p: Array<String>, g: IntArray) {
        super.onRequestPermissionsResult(rc, p, g)
        if (rc == REQ_STORAGE) runInit(pendingAction ?: {})
    }

    private fun runInit(onReady: () -> Unit) {
        log("ROM 로드 중...")
        Thread {
            val ok = engine.initSynth()
            runOnUiThread {
                if (ok) { synthReady = true; log("✅ munt 초기화 완료"); onReady() }
                else log("❌ ROM 없음: /sdcard/Download/munt_roms/ 확인")
            }
        }.start()
    }

    private fun updateStats() {
        val raw = try { engine.getStats() } catch (_: Exception) { return }
        val map = raw.lines().filter { ':' in it }
            .associate { it.substringBefore(':') to it.substringAfter(':') }
        val states = map["partStates"]?.toLongOrNull() ?: 0L
        val names  = map["names"]?.split(",") ?: emptyList()
        tvCounter.text = "MIDI:${map["midi"]?:"0"}  SysEx:${map["sysex"]?:"0"}  ${if(map["active"]=="1")"▶"else"■"}"
        for (i in 0..8) {
            val on = (states shr i) and 1L != 0L; val rhy = i == 8
            leds[i]?.setBackgroundColor(when {
                on && rhy -> LED_RHY_ON; on -> LED_ON; rhy -> LED_RHY_OFF; else -> LED_OFF
            })
            patches[i]?.text = names.getOrElse(i) { "---" }
        }
    }

    override fun onResume()  { super.onResume();  handler.post(statsRunnable) }
    override fun onPause()   { super.onPause();   handler.removeCallbacks(statsRunnable) }
    override fun onDestroy() { super.onDestroy(); engine.stop() }

    private fun log(msg: String) {
        tvLog.append("$msg\n")
        val lines = tvLog.text.lines()
        if (lines.size > 60) tvLog.text = lines.takeLast(60).joinToString("\n") + "\n"
        svLog.post { svLog.fullScroll(ScrollView.FOCUS_DOWN) }
    }
}
