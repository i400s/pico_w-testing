# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

set(PICO_BOARD pico_w CACHE STRING "Board type")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

if (PICO_SDK_VERSION_STRING VERSION_LESS "1.4.0")
  message(FATAL_ERROR "Raspberry Pi Pico SDK version 1.4.0 (or later) required. Your version is ${PICO_SDK_VERSION_STRING}")
endif()

project(wifi_blinkwifigpio C CXX ASM)

# Slightly modified version from wifi examples as vscode sets both the ENV and CMake variable.
if (DEFINED ENV{WIFI_SSID})
  set(WIFI_SSID $ENV{WIFI_SSID})
  message("Using WIFI_SSID from environment ('${WIFI_SSID}')")
else()
  message(FATAL_ERROR "WIFI SSID not supplied")
endif()

if (DEFINED ENV{WIFI_PASSWORD})
  set(WIFI_PASSWORD $ENV{WIFI_PASSWORD})
  message("Using WIFI_PASSWORD from environment ('${WIFI_PASSWORD}')")
else()
  message(FATAL_ERROR "WIFI PASSWORD not supplied")
endif()

set(WIFI_SSID "${WIFI_SSID}" CACHE INTERNAL "Hard coded WiFi SSID")
set(WIFI_PASSWORD "${WIFI_PASSWORD}" CACHE INTERNAL "Hard coded WiFi password")

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

add_compile_definitions("DBGPAUSE=$<CONFIG:Debug>"
  WIFI_SSID=\"${WIFI_SSID}\"
  WIFI_PASSWORD=\"${WIFI_PASSWORD}\"
  )

add_compile_options(-Werror=implicit-function-declaration)

# Add executable. Default name is the project name, version 0.1
add_executable(${PROJECT_NAME}
        src/cyw43_blink_led.c
        src/mcp9808.c
        src/ntp_cyw43.c
        src/msp2807.c
        src/wifi_blinkwifigpio.c
        )
pico_set_program_name(${PROJECT_NAME} "wifi_blinkwifigpio")
pico_set_program_version(${PROJECT_NAME} "0.1")

# Allow stdio to ports
pico_enable_stdio_uart(${PROJECT_NAME} 1)
# pico_enable_stdio_usb(${PROJECT_NAME} 1)

# Add the standard library to the build
target_link_libraries(${PROJECT_NAME}
        pico_stdlib)

# Add the standard include files to the build
target_include_directories(${PROJECT_NAME} PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}
  ${CMAKE_CURRENT_LIST_DIR}/include # Moved lwipopts.h from /.. to /include
  )

# Add any user requested libraries
target_link_libraries(wifi_blinkwifigpio
        # pico_cyw43_arch_none  This is no longer required. It conflicts with threadsafe
        hardware_rtc # Pull in additional rtc support
        hardware_pwm # Pull in pwm control
        hardware_i2c # Pull in I2C control
        pico_cyw43_arch_lwip_threadsafe_background
        )

# Use 'no_flash' to not write to flash storage
# 'copy_to_ram' to load into ram from flash.
# 'default' for execute in place flash
# 'blocked_ram' for something I don't understand.
pico_set_binary_type(${PROJECT_NAME} default)

pico_add_extra_outputs(${PROJECT_NAME})

