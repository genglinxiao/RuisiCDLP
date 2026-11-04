cmake_minimum_required(VERSION 3.15)

if(POLICY CMP0091)
  cmake_policy(SET CMP0091 NEW)
endif()

option(USB_DLP_BUILD_TESTS "Build native conformance tests" ON)
set(USB_DLP_TARGET_PROFILE "host" CACHE STRING "host or a legacy XP/Windows 7/Windows 10 architecture profile")
set_property(CACHE USB_DLP_TARGET_PROFILE PROPERTY STRINGS host legacy-xp-x86 legacy-win7-x86 legacy-win7-x64 legacy-win10-x86 legacy-win10-x64)
set(USB_DLP_XP_ENFORCEMENT "cm" CACHE STRING "XP enforcement experiment: cm, setupapi, or filter")
set_property(CACHE USB_DLP_XP_ENFORCEMENT PROPERTY STRINGS cm setupapi filter)
if(USB_DLP_TARGET_PROFILE STREQUAL "legacy-xp-x86")
  if(USB_DLP_XP_ENFORCEMENT STREQUAL "setupapi")
    add_compile_definitions(USB_DLP_XP_SETUPAPI=1)
  elseif(USB_DLP_XP_ENFORCEMENT STREQUAL "filter")
    add_compile_definitions(USB_DLP_XP_FILTER=1)
  elseif(NOT USB_DLP_XP_ENFORCEMENT STREQUAL "cm")
    message(FATAL_ERROR "Unsupported XP enforcement backend: ${USB_DLP_XP_ENFORCEMENT}")
  endif()
endif()

# The current Visual C++ redistributable does not run on XP. Keep the XP
# package self-contained by linking the supported v141_xp runtime statically.
if(USB_DLP_TARGET_PROFILE STREQUAL "legacy-xp-x86")
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

project(RuisiUsbDlpLegacy VERSION 1.1.0 LANGUAGES CXX)

if(MSVC)
  add_compile_options(/W4 /WX /Zc:__cplusplus /EHsc)
  add_definitions(-DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX)
  if(USB_DLP_TARGET_PROFILE STREQUAL "legacy-xp-x86")
    # The Windows 7.1A objbase.h templates do not compile under /permissive-.
    add_compile_options(/permissive)
    add_definitions(-D_WIN32_WINNT=0x0501 -DWINVER=0x0501 -DNTDDI_VERSION=0x05010300)
    set(USB_DLP_SUBSYSTEM_VERSION "5.01")
  elseif(USB_DLP_TARGET_PROFILE MATCHES "legacy-win7")
    add_compile_options(/permissive-)
    add_definitions(-D_WIN32_WINNT=0x0601 -DWINVER=0x0601 -DNTDDI_VERSION=0x06010000)
    set(USB_DLP_SUBSYSTEM_VERSION "6.01")
  elseif(USB_DLP_TARGET_PROFILE MATCHES "legacy-win10")
    add_compile_options(/permissive-)
    add_definitions(-D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DNTDDI_VERSION=0x0A000000)
    set(USB_DLP_SUBSYSTEM_VERSION "6.00")
  else()
    add_compile_options(/permissive-)
  endif()
endif()

add_library(usb_dlp_legacy_core STATIC
  src/json.cpp
  src/policy.cpp
  src/policy_json.cpp
)
target_include_directories(usb_dlp_legacy_core PUBLIC include)
target_compile_features(usb_dlp_legacy_core PUBLIC cxx_std_14)

if(WIN32)
  set(USB_DLP_CONFIG_MANAGER_LIBRARY cfgmgr32)
  if(USB_DLP_TARGET_PROFILE STREQUAL "legacy-xp-x86")
    # In the 7.1A SDK the CM_* imports are provided by setupapi.lib.
    set(USB_DLP_CONFIG_MANAGER_LIBRARY "")
  endif()

  add_library(usb_dlp_legacy_windows STATIC
    src/windows/http_client.cpp
    src/windows/agent_api.cpp
    src/windows/notifier_bridge.cpp
    src/windows/secure_store.cpp
    src/windows/device_enumerator.cpp
    src/windows/device_enforcer.cpp
  )
  target_include_directories(usb_dlp_legacy_windows PUBLIC include)
  target_link_libraries(usb_dlp_legacy_windows PUBLIC usb_dlp_legacy_core setupapi ${USB_DLP_CONFIG_MANAGER_LIBRARY} advapi32 crypt32 shell32 winhttp ole32 netapi32 userenv wtsapi32)
  target_compile_features(usb_dlp_legacy_windows PUBLIC cxx_std_14)

  add_executable(UsbDlp.LegacyAgent src/windows/agent_main.cpp)
  target_link_libraries(UsbDlp.LegacyAgent PRIVATE usb_dlp_legacy_windows)
  target_compile_definitions(UsbDlp.LegacyAgent PRIVATE USB_DLP_PROFILE="${USB_DLP_TARGET_PROFILE}")
  string(TIMESTAMP USB_DLP_BUILD_STAMP "%Y-%m-%dT%H:%MZ" UTC)
  target_compile_definitions(UsbDlp.LegacyAgent PRIVATE USB_DLP_BUILD_STAMP="${USB_DLP_BUILD_STAMP}")

  add_executable(UsbDlp.LegacyNotifier WIN32 src/windows/notifier_main.cpp)
  target_link_libraries(UsbDlp.LegacyNotifier PRIVATE usb_dlp_legacy_windows)

  add_executable(UsbDlp.LegacyServer src/windows/server_main.cpp)
  target_link_libraries(UsbDlp.LegacyServer PRIVATE usb_dlp_legacy_windows ws2_32)

  if(USB_DLP_TARGET_PROFILE STREQUAL "legacy-xp-x86" AND USB_DLP_XP_ENFORCEMENT STREQUAL "filter")
    add_executable(UsbDlp.XpFilterSetup src/windows/xp_filter_setup.cpp)
    target_include_directories(UsbDlp.XpFilterSetup PRIVATE include)
    target_link_libraries(UsbDlp.XpFilterSetup PRIVATE advapi32)
    target_link_options(UsbDlp.XpFilterSetup PRIVATE /SUBSYSTEM:CONSOLE,${USB_DLP_SUBSYSTEM_VERSION})
  endif()

  if(USB_DLP_SUBSYSTEM_VERSION)
    target_link_options(UsbDlp.LegacyAgent PRIVATE /SUBSYSTEM:CONSOLE,${USB_DLP_SUBSYSTEM_VERSION})
    target_link_options(UsbDlp.LegacyNotifier PRIVATE /SUBSYSTEM:WINDOWS,${USB_DLP_SUBSYSTEM_VERSION})
    target_link_options(UsbDlp.LegacyServer PRIVATE /SUBSYSTEM:CONSOLE,${USB_DLP_SUBSYSTEM_VERSION})
  endif()
endif()

if(USB_DLP_BUILD_TESTS)
  enable_testing()
  add_executable(usb_dlp_legacy_policy_tests tests/policy_tests.cpp)
  target_link_libraries(usb_dlp_legacy_policy_tests PRIVATE usb_dlp_legacy_core)
  add_test(NAME T-M10-01-policy-parity COMMAND usb_dlp_legacy_policy_tests)
  if(WIN32)
    add_executable(usb_dlp_legacy_store_tests tests/secure_store_tests.cpp)
    target_link_libraries(usb_dlp_legacy_store_tests PRIVATE usb_dlp_legacy_windows)
    add_test(NAME T-M10-05-secure-store COMMAND usb_dlp_legacy_store_tests)
  endif()
endif()
