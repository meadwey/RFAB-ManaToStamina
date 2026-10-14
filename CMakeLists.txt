cmake_minimum_required(VERSION 3.19)

project(RFABManaToStamina VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
FetchContent_Declare(
    CommonLibSSE
    GIT_REPOSITORY https://github.com/powerof3/CommonLibSSE.git
    GIT_TAG        431ded26ff2861c2de2e29f839a7c36f1829c592
)
FetchContent_MakeAvailable(CommonLibSSE)

add_library(RFABManaToStamina SHARED
    src/Main.cpp
    src/ManaToStamina.h
    src/ManaToStamina.cpp
    src/DebugLog.h
    src/PCH.h
)

target_link_libraries(RFABManaToStamina PRIVATE CommonLibSSE::CommonLibSSE)

target_precompile_headers(RFABManaToStamina PRIVATE src/PCH.h)

if (MSVC)
    target_compile_options(RFABManaToStamina PRIVATE /W4 /permissive- /utf-8)
endif()

set_target_properties(RFABManaToStamina PROPERTIES
    PREFIX ""
    OUTPUT_NAME "RFABManaToStamina"
)
