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

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# Add executable. Default name is the project name, version 0.1
add_executable(wifi_blinkwifigpio src/wifi_blinkwifigpio.c )
pico_set_program_name(wifi_blinkwifigpio "wifi_blinkwifigpio")
pico_set_program_version(wifi_blinkwifigpio "0.1")

# Allow stdio to ports
pico_enable_stdio_uart(wifi_blinkwifigpio 1)
pico_enable_stdio_usb(wifi_blinkwifigpio 1)

# Add the standard library to the build
target_link_libraries(wifi_blinkwifigpio
        pico_stdlib)

# Add the standard include files to the build
target_include_directories(wifi_blinkwifigpio PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}
  ${CMAKE_CURRENT_LIST_DIR}/.. # for our common lwipopts or any other standard includes, if required
)

# Add any user requested libraries
target_link_libraries(wifi_blinkwifigpio 
        pico_cyw43_arch_none
        )

# Use 'no_flash' to not write to flash storage
# 'copy_to_ram' to load into ram from flash.
# 'default' for execute in place flash
# 'blocked_ram' for something I don't understand.
pico_set_binary_type(wifi_blinkwifigpio default)

pico_add_extra_outputs(wifi_blinkwifigpio)

