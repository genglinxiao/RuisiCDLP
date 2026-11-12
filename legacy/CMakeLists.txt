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

# The current Visual C++ redistributable does not run on XP, and the fab-side
# machines this product targets are offline (no Windows Update, no VC++ redist).
# Link the CRT statically for every legacy profile so the kit stays self-contained.
if(USB_DLP_TARGET_PROFILE MATCHES "legacy-")
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

project(RuisiUsbDlpLegacy VERSION 1.1.0 LANGUAGES CXX)

if(MSVC)
  # The notifier ships localized strings, so the compiler must read sources as
  # UTF-8 regardless of the build machine's ANSI code page. Without this, a
  # Chinese literal is decoded as GBK and the UI shows mojibake.
  add_compile_options(/utf-8)
  add_compile_options(/W4 /WX /Zc:__cplusplus /EHsc)
  add_definitions(-DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX)
  if(USB_DLP_TARGET_PROFILE STREQUAL "legacy-xp-x86")
    # The Windows 7.1A objbase.h templates do not compile under /permissive-.
    add_compile_options(/permissive)
    # Windows XP has no Device Installation Restrictions policy layer at all.
    # Mark the profile so the passive backstop is excluded at compile time and
    # the XP binary keeps exactly the behaviour it was qualified with.
    add_definitions(-D_WIN32_WINNT=0x0501 -DWINVER=0x0501 -DNTDDI_VERSION=0x05010300
                    -DUSB_DLP_LEGACY_XP=1)
    set(USB_DLP_SUBSYSTEM_VERSION "5.01")
  elseif(USB_DLP_TARGET_PROFILE MATCHES "legacy-win7")
    add_compile_options(/permissive-)
    # Vista+ winsock2.h marks inet_addr/inet_ntoa deprecated; /WX turns that
    # into a build break. The call sites parse a bind address and are correct
    # as written, so silence the SDK deprecation instead of rewriting them.
    add_definitions(-D_WIN32_WINNT=0x0601 -DWINVER=0x0601 -DNTDDI_VERSION=0x06010000
                    -D_WINSOCK_DEPRECATED_NO_WARNINGS)
    set(USB_DLP_SUBSYSTEM_VERSION "6.01")
  elseif(USB_DLP_TARGET_PROFILE MATCHES "legacy-win10")
    add_compile_options(/permissive-)
    add_definitions(-D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DNTDDI_VERSION=0x0A000000
                    -D_WINSOCK_DEPRECATED_NO_WARNINGS)
    set(USB_DLP_SUBSYSTEM_VERSION "6.00")
  else()
    add_compile_options(/permissive-)
  endif()
endif()

add_library(usb_dlp_legacy_core STATIC
  src/json.cpp
  src/policy.cpp
  src/policy_json.cpp
  src/crypto.cpp
  src/approval.cpp
  src/approval_key.cpp
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
  # The passive installation baseline is Vista and newer only. Excluding the
  # translation unit for XP keeps that binary byte-for-byte free of the code path.
  if(NOT USB_DLP_TARGET_PROFILE STREQUAL "legacy-xp-x86")
    target_sources(usb_dlp_legacy_windows PRIVATE src/windows/policy_registry.cpp)
  endif()
  target_include_directories(usb_dlp_legacy_windows PUBLIC include)
  target_link_libraries(usb_dlp_legacy_windows PUBLIC usb_dlp_legacy_core setupapi ${USB_DLP_CONFIG_MANAGER_LIBRARY} advapi32 crypt32 shell32 winhttp ole32 netapi32 userenv wtsapi32)
  target_compile_features(usb_dlp_legacy_windows PUBLIC cxx_std_14)

  add_executable(UsbDlp.LegacyAgent src/windows/agent_main.cpp)
  target_link_libraries(UsbDlp.LegacyAgent PRIVATE usb_dlp_legacy_windows)
  target_compile_definitions(UsbDlp.LegacyAgent PRIVATE USB_DLP_PROFILE="${USB_DLP_TARGET_PROFILE}")
  # The build stamp is produced by the compiler (__DATE__/__TIME__) inside
  # agent_main.cpp. A configure-time timestamp here would go stale on every
  # plain "cmake --build" and make the log name the wrong binary.

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

  add_executable(usb_dlp_legacy_approval_tests tests/approval_tests.cpp)
  target_link_libraries(usb_dlp_legacy_approval_tests PRIVATE usb_dlp_legacy_core)
  add_test(NAME T-M10-07-approval-codec COMMAND usb_dlp_legacy_approval_tests)
  if(WIN32)
    add_executable(usb_dlp_legacy_store_tests tests/secure_store_tests.cpp)
    target_link_libraries(usb_dlp_legacy_store_tests PRIVATE usb_dlp_legacy_windows)
    add_test(NAME T-M10-05-secure-store COMMAND usb_dlp_legacy_store_tests)
  endif()
endif()
