# Tales of the World: Radiant Mythology 2 -- PSPRecomp profile wiring.
#
# This profile is bootstrap-safe on purpose: the decrypted EBOOT, the generated
# AOT corpus and the profile host are local inputs and may not exist yet. A
# configure with -DPSPRECOMP_PROFILE=tow-2 therefore succeeds with a status
# message, and Tow2Native is added as soon as generated/*.cpp and host/*.cpp
# are present (file(GLOB CONFIGURE_DEPENDS) picks them up on the next build).

set(TOW2_PROFILE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

file(GLOB TOW2_GENERATED CONFIGURE_DEPENDS "${TOW2_PROFILE_DIR}/generated/*.cpp")
file(GLOB TOW2_HOST_SOURCES CONFIGURE_DEPENDS
    "${TOW2_PROFILE_DIR}/host/*.cpp"
    "${TOW2_PROFILE_DIR}/host/ge/*.cpp")
file(GLOB TOW2_MODULE_GENERATED CONFIGURE_DEPENDS
    "${TOW2_PROFILE_DIR}/generated_libfile/*.cpp")

if(NOT TOW2_GENERATED)
    message(STATUS
        "tow-2: generated/ is empty; Tow2Native is not part of this build.\n"
        "       Generate the corpus with:\n"
        "         psp_recomp profiles/tow-2/game/eboot.dec --auto profiles/tow-2/generated 0x08804000 0x2000\n"
        "       (or: cmake --build <build-dir> --target tow2_generate)")
endif()
if(NOT TOW2_HOST_SOURCES)
    message(STATUS "tow-2: host/ has no translation units yet; Tow2Native is not part of this build.")
endif()

# ---------------------------------------------------------------------------
# Corpus compile target
#
# Compiles every generated unit once and feeds those objects to Tow2Native.
# It also works without host/ exists, which makes it the Phase A CI gate.
# ---------------------------------------------------------------------------
if(TOW2_GENERATED)
    add_library(Tow2Corpus OBJECT ${TOW2_GENERATED})
    target_include_directories(Tow2Corpus PRIVATE
        "${TOW2_PROFILE_DIR}/generated")
    target_link_libraries(Tow2Corpus PRIVATE psprecomp_core)

    if(MSVC)
        target_compile_definitions(Tow2Corpus PRIVATE _CRT_SECURE_NO_WARNINGS)
        if(PSPRECOMP_GENERATED_OPT_LEVEL STREQUAL "0")
            set(TOW2_CORPUS_OPT "/Od")
        elseif(PSPRECOMP_GENERATED_OPT_LEVEL STREQUAL "3")
            set(TOW2_CORPUS_OPT "/Ox")
        else()
            set(TOW2_CORPUS_OPT "/O${PSPRECOMP_GENERATED_OPT_LEVEL}")
        endif()
        # Keep generated units at /Ob0 for the first bring-up: several ToW-sized
        # units reach gigabytes of peak compiler memory once inlining is raised.
        set_source_files_properties(${TOW2_GENERATED} PROPERTIES COMPILE_OPTIONS
            "${TOW2_CORPUS_OPT};/Ob0;/bigobj;/EHsc;/W0;${PSPRECOMP_MSVC_MP_FLAG}")
    else()
        set_source_files_properties(${TOW2_GENERATED} PROPERTIES COMPILE_OPTIONS
            "-O2;-g0")
    endif()
endif()

# Secondary module corpus: libfile.prx is a plain ELF and is executed at
# 0x08000000 instead of being HLE-stubbed. Its generated symbols are prefixed
# with "libfile_" so they can coexist with the main EBOOT corpus.
if(TOW2_MODULE_GENERATED)
    add_library(Tow2ModuleCorpus OBJECT ${TOW2_MODULE_GENERATED})
    target_include_directories(Tow2ModuleCorpus PRIVATE
        "${TOW2_PROFILE_DIR}/generated_libfile")
    target_link_libraries(Tow2ModuleCorpus PRIVATE psprecomp_core)
    if(MSVC)
        target_compile_definitions(Tow2ModuleCorpus PRIVATE _CRT_SECURE_NO_WARNINGS)
        set_source_files_properties(${TOW2_MODULE_GENERATED} PROPERTIES COMPILE_OPTIONS
            "${TOW2_CORPUS_OPT};/Ob0;/bigobj;/EHsc;/W0;${PSPRECOMP_MSVC_MP_FLAG}")
    else()
        set_source_files_properties(${TOW2_MODULE_GENERATED} PROPERTIES COMPILE_OPTIONS
            "-O2;-g0")
    endif()
endif()

set(TOW2_MODULE_OBJECTS "")
if(TARGET Tow2ModuleCorpus)
    set(TOW2_MODULE_OBJECTS $<TARGET_OBJECTS:Tow2ModuleCorpus>)
endif()

if(TOW2_GENERATED AND TOW2_HOST_SOURCES)
    add_executable(Tow2Native
        ${TOW2_HOST_SOURCES}
        $<TARGET_OBJECTS:Tow2Corpus>
        ${TOW2_MODULE_OBJECTS})

    target_include_directories(Tow2Native PRIVATE
        "${TOW2_PROFILE_DIR}/host"
        "${TOW2_PROFILE_DIR}/host/ge"
        "${TOW2_PROFILE_DIR}/generated")

    # FFmpeg is used by the profile's PSMF/MPEG and ATRAC3+ players. The
    # shared LGPL build from WinGet includes import libraries and DLLs; set
    # FFMPEG_ROOT to override auto-detection.
    set(FFMPEG_ROOT "" CACHE PATH "FFmpeg installation root")
    if(NOT FFMPEG_ROOT)
        if(DEFINED ENV{FFMPEG_ROOT})
            set(FFMPEG_ROOT "$ENV{FFMPEG_ROOT}")
        else()
            file(GLOB _ffmpeg_headers
                "$ENV{LOCALAPPDATA}/Microsoft/WinGet/Packages/BtbN.FFmpeg*/*/include/libavcodec/avcodec.h")
            if(_ffmpeg_headers)
                list(GET _ffmpeg_headers 0 _ffmpeg_header)
                get_filename_component(_ffmpeg_include "${_ffmpeg_header}" DIRECTORY)
                get_filename_component(_ffmpeg_include "${_ffmpeg_include}" DIRECTORY)
                get_filename_component(FFMPEG_ROOT "${_ffmpeg_include}" DIRECTORY)
            endif()
        endif()
    endif()

    find_path(FFMPEG_INCLUDE_DIR libavcodec/avcodec.h HINTS "${FFMPEG_ROOT}/include")
    find_library(FFMPEG_AVFORMAT_LIBRARY avformat HINTS "${FFMPEG_ROOT}/lib")
    find_library(FFMPEG_AVCODEC_LIBRARY avcodec HINTS "${FFMPEG_ROOT}/lib")
    find_library(FFMPEG_AVUTIL_LIBRARY avutil HINTS "${FFMPEG_ROOT}/lib")
    find_library(FFMPEG_SWSCALE_LIBRARY swscale HINTS "${FFMPEG_ROOT}/lib")
    find_library(FFMPEG_SWRESAMPLE_LIBRARY swresample HINTS "${FFMPEG_ROOT}/lib")
    if(NOT FFMPEG_INCLUDE_DIR OR NOT FFMPEG_AVFORMAT_LIBRARY OR
       NOT FFMPEG_AVCODEC_LIBRARY OR NOT FFMPEG_AVUTIL_LIBRARY OR
       NOT FFMPEG_SWSCALE_LIBRARY OR NOT FFMPEG_SWRESAMPLE_LIBRARY)
        message(FATAL_ERROR
            "FFmpeg headers/libraries not found. Set FFMPEG_ROOT to an FFmpeg "
            "shared build containing include/ and lib/.")
    endif()

    target_include_directories(Tow2Native PRIVATE "${FFMPEG_INCLUDE_DIR}")
    target_link_libraries(Tow2Native PRIVATE
        psprecomp_core
        "${FFMPEG_AVFORMAT_LIBRARY}"
        "${FFMPEG_AVCODEC_LIBRARY}"
        "${FFMPEG_AVUTIL_LIBRARY}"
        "${FFMPEG_SWSCALE_LIBRARY}"
        "${FFMPEG_SWRESAMPLE_LIBRARY}")

    if(WIN32 AND FFMPEG_ROOT AND EXISTS "${FFMPEG_ROOT}/bin")
        file(GLOB FFMPEG_RUNTIME_DLLS "${FFMPEG_ROOT}/bin/*.dll")
        if(FFMPEG_RUNTIME_DLLS)
            add_custom_command(TARGET Tow2Native POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${FFMPEG_RUNTIME_DLLS} "$<TARGET_FILE_DIR:Tow2Native>"
                COMMENT "Copying FFmpeg runtime DLLs")
        endif()
    endif()

    if(MSVC)
        target_compile_definitions(Tow2Native PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_compile_options(Tow2Native PRIVATE /W4 /EHsc ${PSPRECOMP_MSVC_MP_FLAG})
    else()
        target_compile_options(Tow2Native PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    psprecomp_enable_host_avx(Tow2Native)

    set_target_properties(Tow2Native PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
        RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/bin")
endif()

# ---------------------------------------------------------------------------
# AOT corpus generation
#
# The corpus is a checked-in profile artifact once generated; this target only
# re-creates it from the user's own decrypted executable. It is not part of ALL.
# ---------------------------------------------------------------------------
set(PSPRECOMP_TOW2_EBOOT "${TOW2_PROFILE_DIR}/game/eboot.dec" CACHE FILEPATH
    "Decrypted Tales of the World: Radiant Mythology 2 executable")
set(PSPRECOMP_TOW2_LOAD_BASE "0x08804000" CACHE STRING
    "Guest load base for the tow-2 AOT corpus")
set(PSPRECOMP_TOW2_UNIT_SPAN "0x2000" CACHE STRING
    "Bytes per generated tow-2 AOT unit")

if(NOT EXISTS "${PSPRECOMP_TOW2_EBOOT}")
    message(STATUS
        "tow-2: executable not present yet (${PSPRECOMP_TOW2_EBOOT}).\n"
        "       tow2_generate requires the decrypted EBOOT and the framework psp_recomp tool.")
endif()

add_custom_target(tow2_generate
    COMMAND $<TARGET_FILE:psp_recomp> "${PSPRECOMP_TOW2_EBOOT}"
            --auto "${TOW2_PROFILE_DIR}/generated"
            "${PSPRECOMP_TOW2_LOAD_BASE}" "${PSPRECOMP_TOW2_UNIT_SPAN}"
    DEPENDS psp_recomp
    COMMENT "Generating tow-2 AOT corpus from ${PSPRECOMP_TOW2_EBOOT}"
    VERBATIM)
