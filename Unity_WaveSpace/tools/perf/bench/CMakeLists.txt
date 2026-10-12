cmake_minimum_required(VERSION 3.20)
project(at_spat_perf_bench LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

if(MSVC)
    # Release représentatif du moteur : /O2, AVX2, runtime DLL non-debug
    add_compile_options("$<$<CONFIG:Release>:/O2;/Ob2;/arch:AVX2>" /W3 /utf-8)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options("$<$<CONFIG:Release>:-O2>" -Wall -Wextra)
endif()

# Banc de charge : charge la DLL du moteur dynamiquement (aucun lien statique)
add_executable(at_spat_bench main.cpp at_spat_api.h ../engine/At_PerfProbe.h)
target_compile_definitions(at_spat_bench PRIVATE AT_SPAT_PERF_PROBE=1)
if(WIN32)
    target_link_libraries(at_spat_bench PRIVATE winmm)
else()
    target_link_libraries(at_spat_bench PRIVATE dl)
endif()

# Micro-bench autonome (plafond théorique, dénormaux, P vs E cores)
add_executable(at_microbench microbench.cpp)
