package com.example.muntforandroid

/**
 * 연속 MIDI 바이트 스트림 → 완성된 메시지 콜백
 * Running Status 지원, SysEx 지원 (F0..F7)
 * USB-serial 38400bps 로 들어오는 raw MIDI 바이트용
 */
class MidiStreamParser(private val onMessage: (ByteArray) -> Unit) {

    private var runningStatus = 0
    private val buf = ByteArray(512)
    private var bufPos = 0
    private var expected = 0  // 현재 메시지에서 기대하는 총 바이트 수
    private var inSysEx = false

    fun reset() { runningStatus=0; bufPos=0; expected=0; inSysEx=false }

    fun feed(data: ByteArray) {
        for (b in data) feedByte(b.toInt() and 0xFF)
    }

    private fun feedByte(b: Int) {
        // 실시간 메시지 (F8~FF) → 버퍼 상태 무관하게 즉시 전달
        if (b >= 0xF8) { onMessage(byteArrayOf(b.toByte())); return }

        // SysEx 종료
        if (b == 0xF7) {
            if (inSysEx && bufPos > 0) {
                buf[bufPos++] = 0xF7.toByte()
                onMessage(buf.copyOf(bufPos))
            }
            inSysEx = false; bufPos = 0; expected = 0
            return
        }

        // SysEx 시작
        if (b == 0xF0) {
            inSysEx = true; bufPos = 0; expected = 0
            buf[bufPos++] = 0xF0.toByte()
            return
        }

        // SysEx 데이터 수집
        if (inSysEx) {
            if (bufPos < buf.size) buf[bufPos++] = b.toByte()
            return
        }

        // 상태 바이트
        if (b and 0x80 != 0) {
            runningStatus = b; bufPos = 0
            buf[bufPos++] = b.toByte()
            expected = messageLength(b)
            if (expected == 1) { emit(); return }
            return
        }

        // 데이터 바이트: Running Status 적용
        if (runningStatus == 0) return  // 상태 바이트 없이 데이터 → 무시
        if (bufPos == 0) {              // Running Status 재활성
            buf[bufPos++] = runningStatus.toByte()
            expected = messageLength(runningStatus)
        }
        if (bufPos < buf.size) buf[bufPos++] = b.toByte()
        if (bufPos >= expected) emit()
    }

    private fun emit() {
        onMessage(buf.copyOf(bufPos))
        // Running Status: 상태 바이트 유지, 다음 메시지를 위해 bufPos=0
        bufPos = 0
    }

    private fun messageLength(status: Int): Int = when (status and 0xF0) {
        0x80, 0x90, 0xA0, 0xB0, 0xE0 -> 3
        0xC0, 0xD0                    -> 2
        0xF0 -> when (status) { 0xF2 -> 3; 0xF1, 0xF3 -> 2; else -> 1 }
        else -> 1
    }
}
