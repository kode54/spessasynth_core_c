cmake_minimum_required(VERSION 3.21)
project(spessasynth_core VERSION 4.2.12 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ── Options ──────────────────────────────────────────────────────────────────
option(SS_ENABLE_SF3_VORBIS  "Enable SF3 OGG Vorbis decoding via libvorbisfile" ON)
option(SS_ENABLE_SF3_FLAC    "Enable SF3 FLAC decoding via libFLAC"             ON)
option(SS_BUILD_SHARED       "Build as a shared library"                        ON)
option(SS_BUILD_EXAMPLES     "Build example tools"                              ON)

# ── Core source list ─────────────────────────────────────────────────────────
set(SS_SOURCES
    src/utils/byte_functions.c
    src/utils/file.c
    src/utils/riff_chunk.c
    src/utils/sync_primitives.c
    src/utils/wav_writer.c
    src/midi/midi_loader.c
    src/midi/midi_writer.c
    src/midi/midi_emidi.c
    src/midi/parsers/smf.c
    src/midi/parsers/rmidi.c
    src/midi/parsers/mus.c
    src/midi/parsers/mids.c
    src/midi/parsers/xmi.c
    src/midi/parsers/gmf.c
    src/midi/parsers/hmp.c
    src/midi/parsers/hmi.c
    src/midi/parsers/lds.c
    src/soundbank/soundbank.c
    src/soundbank/dls_reader.c
    src/soundbank/soundfont_reader.c
    src/synthesizer/unit_converter.c
    src/synthesizer/volume_envelope.c
    src/synthesizer/modulation_envelope.c
    src/synthesizer/lowpass_filter.c
    src/synthesizer/wavetable_oscillator.c
    src/synthesizer/voice.c
    src/synthesizer/midi_channel.c
    src/synthesizer/dynamic_modulators.c
    src/synthesizer/processor.c
    src/synthesizer/dsp/chorus.c
    src/synthesizer/dsp/dattorro_reverb.c
    src/synthesizer/dsp/delay_line.c
    src/synthesizer/dsp/reverb.c
    src/synthesizer/dsp/delay.c
    src/synthesizer/dsp/insertion.c
    src/sequencer/sequencer.c
    src/sflist/sflist.c
    extern/json/json.c
    extern/json/json-builder.c
)

# ── Library target ────────────────────────────────────────────────────────────
if(SS_BUILD_SHARED)
    add_library(spessasynth SHARED ${SS_SOURCES})
else()
    add_library(spessasynth STATIC ${SS_SOURCES})
endif()

set_target_properties(spessasynth PROPERTIES VERSION 4.2.12 SOVERSION 0)

target_include_directories(spessasynth
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/include/spessasynth/sflist
            ${CMAKE_CURRENT_SOURCE_DIR}/extern/json
)

# ── stb_vorbis (header-only; compiled as a unity .c translation unit) ────────
if(SS_ENABLE_SF3_VORBIS)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(Vorbis REQUIRED vorbisfile)
        if(Vorbis_FOUND)
            target_sources(spessasynth PRIVATE src/soundbank/vorbis_decode.c)
            include_directories(${Vorbis_INCLUDE_DIRS})
            link_directories(${Vorbis_LIBRARY_DIRS})
            target_link_libraries(spessasynth PRIVATE ${Vorbis_LIBRARIES})
            target_compile_definitions(spessasynth PRIVATE SS_HAVE_LIBVORBISFILE=1)
            message(STATUS "libvorbisfile found")
        else()
            message(STATUS "libvorbisfile not found - Vorbis SF3 sample support disabled.")
        endif()
    else()
        message(STATUS "pkgconfing not found - Vorbis SF3 sample support disabled.")
    endif()
endif()

# ── libFLAC ──────────────────────────────────────────────────────────────────
if(SS_ENABLE_SF3_FLAC)
    find_package(Threads QUIET)
    find_package(FLAC QUIET)
    if(FLAC_FOUND)
        target_sources(spessasynth PRIVATE src/soundbank/flac_decode.c)
        target_link_libraries(spessasynth PRIVATE FLAC::FLAC)
        target_compile_definitions(spessasynth PRIVATE SS_HAVE_LIBFLAC=1)
        message(STATUS "libFLAC found")
    else()
        find_package(PkgConfig QUIET)
        if(PKG_CONFIG_FOUND)
            pkg_check_modules(FLAC flac)
        endif()
        if(FLAC_FOUND)
            target_sources(spessasynth PRIVATE src/soundbank/flac_decode.c)
            target_include_directories(spessasynth PRIVATE ${FLAC_INCLUDE_DIRS})
            target_link_libraries(spessasynth PRIVATE ${FLAC_LIBRARIES})
            target_compile_definitions(spessasynth PRIVATE SS_HAVE_LIBFLAC=1)
        else()
            message(WARNING "libFLAC not found — FLAC SF3 sample support disabled.")
        endif()
    endif()
endif()

# ── Math ─────────────────────────────────────────────────────────────────────
if(NOT MSVC)
target_link_libraries(spessasynth PRIVATE m)
endif()

# ── Warnings ─────────────────────────────────────────────────────────────────
if(MSVC)
    target_compile_options(spessasynth PRIVATE /W3)
else()
    target_compile_options(spessasynth PRIVATE -Wall -Wextra -Wno-unused-parameter -pedantic)
endif()

# ── Examples ─────────────────────────────────────────────────────────────────
if(SS_BUILD_EXAMPLES)
    add_executable(spessasynth-example examples/example.c)
    target_link_libraries(spessasynth-example PRIVATE spessasynth)
    if(NOT MSVC)
        target_link_libraries(spessasynth-example PRIVATE m)
    endif()
    target_include_directories(spessasynth-example PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/examples
    )
    if(NOT MSVC)
        target_compile_options(spessasynth-example PRIVATE -Wall -Wextra -Wno-unused-parameter -pedantic)
    endif()
endif()

# ── Install ──────────────────────────────────────────────────────────────────
include(GNUInstallDirs)
install(TARGETS spessasynth
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
