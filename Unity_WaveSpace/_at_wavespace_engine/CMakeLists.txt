cmake_minimum_required(VERSION 3.22)

project(at_wavespace_engine VERSION 1.0.0 LANGUAGES CXX C)

# ==============================================================================
# JUCE path
# ==============================================================================
if(APPLE)
    set(JUCE_PATH "$ENV{HOME}/JUCE")
elseif(WIN32)
    set(JUCE_PATH "C:/JUCE")
endif()

if(NOT EXISTS "${JUCE_PATH}/CMakeLists.txt")
    message(FATAL_ERROR "JUCE introuvable dans : ${JUCE_PATH}")
endif()

add_subdirectory("${JUCE_PATH}" "${CMAKE_BINARY_DIR}/JUCE" EXCLUDE_FROM_ALL)

# ==============================================================================
# macOS : Universal Binary (x86_64 + arm64) + Big Sur minimum
# Ces variables DOIVENT être définies avant le premier appel à project()
# idéalement, mais les forcer ici via CACHE fonctionne aussi.
# ==============================================================================
if(APPLE)
    set(CMAKE_OSX_ARCHITECTURES   "x86_64;arm64" CACHE STRING "Universal Binary" FORCE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0"        CACHE STRING "macOS Big Sur"    FORCE)
endif()

# ==============================================================================
# Engine source files (shared between the library and the console app)
# ==============================================================================
set(AT_ENGINE_SOURCES
    Source/PluginInterface.cpp
    Source/AT_AudioManager.cpp
    Source/AT_SpatializationEngine.cpp
    Source/AT_SpatPlayer.cpp
    Source/AT_Spatializer.cpp
    Source/AT_BinauralSimpleSpatializer.cpp
    Source/HRTFProcessor.cpp
    Source/SOFAReader.cpp
    Source/UnityLogger.cpp
)

# ==============================================================================
# Cible : bibliothèque partagée (dylib / dll)
# ==============================================================================
add_library(at_wavespace_engine SHARED ${AT_ENGINE_SOURCES})

# ==============================================================================
# Headers — added for IDE visibility (Xcode / Visual Studio file navigator)
# Not compiled, but listed so they appear in the project tree.
# ==============================================================================
file(GLOB AT_HEADERS CONFIGURE_DEPENDS "Source/*.h")
file(GLOB AT_CPPS    CONFIGURE_DEPENDS "Source/*.cpp")

# Build an ordered list: for each .cpp, prepend the matching .h if it exists.
# This gives h-before-cpp ordering in IDEs that respect pbxproj/vcxproj order (Xcode).
set(AT_SOURCES_IDE)
foreach(_cpp ${AT_CPPS})
    get_filename_component(_base ${_cpp} NAME_WE)
    get_filename_component(_dir  ${_cpp} DIRECTORY)
    set(_h "${_dir}/${_base}.h")
    if(_h IN_LIST AT_HEADERS)
        list(APPEND AT_SOURCES_IDE ${_h})
    endif()
    list(APPEND AT_SOURCES_IDE ${_cpp})
endforeach()
# Append any standalone headers with no matching .cpp (e.g. AT_SpatConfig.h)
foreach(_h ${AT_HEADERS})
    if(NOT _h IN_LIST AT_SOURCES_IDE)
        list(APPEND AT_SOURCES_IDE ${_h})
    endif()
endforeach()

target_sources(at_wavespace_engine PRIVATE ${AT_SOURCES_IDE})

# ==============================================================================
# Source groups — IDE virtual folder organisation
# "AT WaveSpace" : engine .h and .cpp together, in declaration order
# "JUCE"         : JUCE module compilation units
# ==============================================================================
source_group("AT WaveSpace" FILES ${AT_SOURCES_IDE})
source_group("JUCE"         REGULAR_EXPRESSION ".*/juce_.*\\.(mm|cpp|c)$")

# Génère JuceHeader.h manuellement dans le dossier de build.
# juce_generate_juce_header ne fonctionne qu'avec juce_add_plugin/juce_add_gui_app.
set(JUCE_HEADER_DIR "${CMAKE_BINARY_DIR}/JuceLibraryCode")
file(MAKE_DIRECTORY "${JUCE_HEADER_DIR}")
file(WRITE "${JUCE_HEADER_DIR}/JuceHeader.h"
[==[
#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_dsp/juce_dsp.h>
#include <juce_events/juce_events.h>
#include <juce_graphics/juce_graphics.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_gui_extra/juce_gui_extra.h>
]==])

target_include_directories(at_wavespace_engine PRIVATE "${JUCE_HEADER_DIR}")

# ==============================================================================
# Standard C++17
# ==============================================================================
target_compile_features(at_wavespace_engine PRIVATE cxx_std_17)

# ==============================================================================
# Définitions de compilation
# ==============================================================================
target_compile_definitions(at_wavespace_engine
    PUBLIC
        JUCE_STRICT_REFCOUNTEDPOINTER=1
        JUCE_STANDALONE_APPLICATION=0
        DISABLE_UNITY_LOGGING
        # Active ASIO sur Windows
        $<$<BOOL:${WIN32}>:JUCE_ASIO=1>
)

# ==============================================================================
# Visibilité des symboles : TOUT exporter (= SYMBOLS_HIDDEN_BY_DEFAULT=NO)
# Nécessaire pour que Unity trouve les fonctions extern "C"
# ==============================================================================
set_target_properties(at_wavespace_engine PROPERTIES
    CXX_VISIBILITY_PRESET     default
    C_VISIBILITY_PRESET       default
    VISIBILITY_INLINES_HIDDEN OFF
)

# ==============================================================================
# Nom du fichier produit
#   macOS : at_wavespace_engine.dylib  (sans préfixe "lib")
#   Win   : at_wavespace_engine.dll    (sans préfixe, comportement par défaut)
# ==============================================================================
set_target_properties(at_wavespace_engine PROPERTIES
    PREFIX ""
    OUTPUT_NAME "at_wavespace_engine"
)

# ==============================================================================
# macOS : install name = @loader_path/at_wavespace_engine.dylib
# Unity charge la dylib depuis son propre dossier → @loader_path est correct
# ==============================================================================
if(APPLE)
    set_target_properties(at_wavespace_engine PROPERTIES
        BUILD_WITH_INSTALL_NAME_DIR TRUE
        INSTALL_NAME_DIR           "@loader_path"
        MACOSX_RPATH               FALSE
    )
endif()

# ==============================================================================
# Windows : ASIO SDK headers
# ==============================================================================
if(WIN32)
    target_include_directories(at_wavespace_engine PRIVATE
        "C:/JUCE/modules/juce_audio_devices/native/ASIOSDK/common"
        "C:/JUCE/modules/juce_audio_devices/native/ASIOSDK/host"
        "C:/JUCE/modules/juce_audio_devices/native/ASIOSDK"
    )
endif()

# ==============================================================================
# Liens JUCE
# ==============================================================================
target_link_libraries(at_wavespace_engine
    PRIVATE
        juce::juce_audio_basics
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_audio_processors
        juce::juce_audio_processors_headless
        juce::juce_audio_utils
        juce::juce_core
        juce::juce_data_structures
        juce::juce_dsp
        juce::juce_events
        juce::juce_graphics
        juce::juce_gui_basics
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

# ==============================================================================
# Post-build : copie vers Assets Unity
# Chemin relatif depuis le dossier du CMakeLists.txt :
#   at_wavespace_engine/
#   └── CMakeLists.txt   ← ici
#   └── Source/
#   └── ...
#   UnityProject/
#   └── Assets/At_WaveSpace/Plugins/...
#
# Arborescence attendue :
#   <racine>/
#   ├── Assets/At_WaveSpace/Plugins/   ← projet Unity
#   └── at_wavespace_engine/
#       └── CMakeLists.txt             ← CMAKE_SOURCE_DIR
#
# CMAKE_SOURCE_DIR/.. remonte d'un niveau = racine commune avec Assets/
# Adaptez si votre arborescence est différente.
# ==============================================================================
set(UNITY_PLUGINS_BASE "${CMAKE_SOURCE_DIR}/../Assets/At_WaveSpace/Plugins")

if(APPLE)
    set(UNITY_OUTPUT "${UNITY_PLUGINS_BASE}/MacOSX")

    add_custom_command(TARGET at_wavespace_engine POST_BUILD
        COMMAND mkdir -p "${UNITY_OUTPUT}"
        COMMAND cp -v
                "$<TARGET_FILE:at_wavespace_engine>"
                "${UNITY_OUTPUT}/"
        COMMAND codesign --force --sign -
                "${UNITY_OUTPUT}/$<TARGET_FILE_NAME:at_wavespace_engine>"
        COMMAND nm -g "${UNITY_OUTPUT}/$<TARGET_FILE_NAME:at_wavespace_engine>"
                | head -20
        COMMAND lipo -info
                "${UNITY_OUTPUT}/$<TARGET_FILE_NAME:at_wavespace_engine>"
        COMMAND echo "Copie, signature et verification terminees"
        COMMENT "→ Copie dylib vers Unity MacOSX plugins"
        VERBATIM
    )

elseif(WIN32)
    set(UNITY_OUTPUT "${UNITY_PLUGINS_BASE}/Win")

    # Copie DLL + PDB (le script CMake gère l'absence du .pdb en Release)
    set(COPY_SCRIPT "${CMAKE_BINARY_DIR}/copy_to_unity.cmake")
    file(WRITE "${COPY_SCRIPT}" [=[
cmake_minimum_required(VERSION 3.22)
file(MAKE_DIRECTORY "${DST}")
file(COPY "${DLL}" DESTINATION "${DST}")
message(STATUS "Copie terminee : ${DLL} -> ${DST}")
if(EXISTS "${PDB}")
    file(COPY "${PDB}" DESTINATION "${DST}")
    message(STATUS "Copie terminee : ${PDB} -> ${DST}")
endif()
]=])

    add_custom_command(TARGET at_wavespace_engine POST_BUILD
        COMMAND ${CMAKE_COMMAND}
                "-DDLL=$<TARGET_FILE:at_wavespace_engine>"
                "-DPDB=$<TARGET_PDB_FILE:at_wavespace_engine>"
                "-DDST=${UNITY_OUTPUT}"
                -P "${COPY_SCRIPT}"
        COMMENT "→ Copie dll vers Unity Win plugins"
        VERBATIM
    )
endif()

# ==============================================================================
# Résumé des paramètres au moment du configure
# ==============================================================================
message(STATUS "------------------------------------------------------------")
message(STATUS "  Projet         : ${PROJECT_NAME} v${PROJECT_VERSION}")
message(STATUS "  JUCE           : ${JUCE_PATH}")
message(STATUS "  Compilateur    : ${CMAKE_CXX_COMPILER_ID}")
if(APPLE)
    message(STATUS "  Architectures  : ${CMAKE_OSX_ARCHITECTURES}")
    message(STATUS "  Deployment     : macOS ${CMAKE_OSX_DEPLOYMENT_TARGET}")
endif()
message(STATUS "  Unity output   : ${UNITY_OUTPUT}")
message(STATUS "------------------------------------------------------------")

# ==============================================================================
# Console Application (optional — enable with -DBUILD_CONSOLE_APP=ON)
# or via the dedicated CMake presets (macos-console-release, etc.)
# ==============================================================================
option(BUILD_CONSOLE_APP "Build the standalone console test application" OFF)

if(BUILD_CONSOLE_APP)

    add_executable(TestAppConsole
        ${AT_ENGINE_SOURCES}
        Source/Main.cpp
    )

    # IDE sources: reuse the ordered h/cpp list, add Main.cpp at the top
    set(CONSOLE_SOURCES_IDE ${AT_SOURCES_IDE})
    set(_main "${CMAKE_CURRENT_SOURCE_DIR}/Source/Main.cpp")
    list(PREPEND CONSOLE_SOURCES_IDE ${_main})

    target_sources(TestAppConsole PRIVATE ${CONSOLE_SOURCES_IDE})
    source_group("AT WaveSpace" FILES ${CONSOLE_SOURCES_IDE})
    source_group("JUCE"         REGULAR_EXPRESSION ".*/juce_.*\\.(mm|cpp|c)$")

    # AT_SPAT_CONSOLE_APP disables async device scanning (requires message loop)
    target_compile_definitions(TestAppConsole
        PRIVATE
            AT_SPAT_CONSOLE_APP
            DISABLE_UNITY_LOGGING
            JUCE_STRICT_REFCOUNTEDPOINTER=1
            JUCE_STANDALONE_APPLICATION=1
            $<$<BOOL:${WIN32}>:JUCE_ASIO=1>
    )

    target_compile_features(TestAppConsole PRIVATE cxx_std_17)
    target_include_directories(TestAppConsole PRIVATE "${JUCE_HEADER_DIR}")

    if(WIN32)
        target_include_directories(TestAppConsole PRIVATE
            "C:/JUCE/modules/juce_audio_devices/native/ASIOSDK/common"
            "C:/JUCE/modules/juce_audio_devices/native/ASIOSDK/host"
            "C:/JUCE/modules/juce_audio_devices/native/ASIOSDK"
        )
    endif()

    target_link_libraries(TestAppConsole
        PRIVATE
            juce::juce_audio_basics
            juce::juce_audio_devices
            juce::juce_audio_formats
            juce::juce_audio_processors
            juce::juce_audio_processors_headless
            juce::juce_audio_utils
            juce::juce_core
            juce::juce_data_structures
            juce::juce_dsp
            juce::juce_events
            juce::juce_graphics
            juce::juce_gui_basics
            juce::juce_gui_extra
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags
    )

    set_target_properties(TestAppConsole PROPERTIES
        OUTPUT_NAME "TestAppConsole"
    )

    message(STATUS "  Console app    : ON (TestAppConsole)")

endif()
