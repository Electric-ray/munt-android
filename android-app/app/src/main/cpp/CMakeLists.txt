cmake_minimum_required(VERSION 3.22.1)
project("muntforandroid")

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── mt32emu 소스 목록 ──────────────────────────────────────────────────
set(MT32EMU_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/mt32emu)

set(MT32EMU_SRCS
    ${MT32EMU_ROOT}/Analog.cpp
    ${MT32EMU_ROOT}/BReverbModel.cpp
    ${MT32EMU_ROOT}/Display.cpp
    ${MT32EMU_ROOT}/File.cpp
    ${MT32EMU_ROOT}/FileStream.cpp
    ${MT32EMU_ROOT}/LA32FloatWaveGenerator.cpp
    ${MT32EMU_ROOT}/LA32Ramp.cpp
    ${MT32EMU_ROOT}/LA32WaveGenerator.cpp
    ${MT32EMU_ROOT}/MidiStreamParser.cpp
    ${MT32EMU_ROOT}/Part.cpp
    ${MT32EMU_ROOT}/Partial.cpp
    ${MT32EMU_ROOT}/PartialManager.cpp
    ${MT32EMU_ROOT}/Poly.cpp
    ${MT32EMU_ROOT}/ROMInfo.cpp
    ${MT32EMU_ROOT}/SampleRateConverter.cpp
    ${MT32EMU_ROOT}/Synth.cpp
    ${MT32EMU_ROOT}/Tables.cpp
    ${MT32EMU_ROOT}/TVA.cpp
    ${MT32EMU_ROOT}/TVF.cpp
    ${MT32EMU_ROOT}/TVP.cpp
    ${MT32EMU_ROOT}/VersionTagging.cpp
    # SHA1
    ${MT32EMU_ROOT}/sha1/sha1.cpp
    # InternalResampler (외부 라이브러리 불필요)
    ${MT32EMU_ROOT}/srchelper/InternalResampler.cpp
    ${MT32EMU_ROOT}/srchelper/srctools/src/FIRResampler.cpp
    ${MT32EMU_ROOT}/srchelper/srctools/src/IIR2xResampler.cpp
    ${MT32EMU_ROOT}/srchelper/srctools/src/LinearResampler.cpp
    ${MT32EMU_ROOT}/srchelper/srctools/src/ResamplerModel.cpp
    ${MT32EMU_ROOT}/srchelper/srctools/src/SincResampler.cpp
)

# 공통 컴파일 정의 (mt32emu 와 JNI 타겟 모두에 필요)
set(MT32EMU_DEFINES
    MT32EMU_EXPORTS_TYPE=0          # C++ API 직접 사용, static build
    MT32EMU_WITH_INTERNAL_RESAMPLER=1  # soxr/libsamplerate 불필요
)

add_library(mt32emu STATIC ${MT32EMU_SRCS})

target_include_directories(mt32emu PUBLIC
    ${MT32EMU_ROOT}
    ${MT32EMU_ROOT}/sha1
    ${MT32EMU_ROOT}/srchelper
    ${MT32EMU_ROOT}/srchelper/srctools/include
)

target_compile_definitions(mt32emu PRIVATE ${MT32EMU_DEFINES})

# ── JNI 브리지 ────────────────────────────────────────────────────────
add_library(muntforandroid SHARED MuntBridge.cpp)

target_compile_definitions(muntforandroid PRIVATE ${MT32EMU_DEFINES})

target_link_libraries(muntforandroid
    mt32emu
    aaudio
    android
    log
)
