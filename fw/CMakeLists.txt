cmake_minimum_required(VERSION 3.20)

project(giris_fw C ASM)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# --- paths ---------------------------------------------------------------
set(BSP     ${CMAKE_SOURCE_DIR}/vendor/at32f402_405)
set(CMSIS   ${BSP}/libraries/cmsis/cm4)
set(DRIVERS ${BSP}/libraries/drivers)
set(TUSB    ${CMAKE_SOURCE_DIR}/vendor/tinyusb)

if(NOT EXISTS ${BSP})
  message(FATAL_ERROR "vendor/ is empty — run tools/fetch-deps.sh first")
endif()

# --- target part ---------------------------------------------------------
# AT32F405RBT7-7: Cortex-M4F @ 216 MHz, 128 KB flash (128 x 1 KB), 70 KB SRAM,
# USB OTG_HS with on-chip PHY (host port J3) + USB OTG_FS (debug port J2).
set(PART AT32F405RBT7_7)

set(MCU_FLAGS
  -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard)

add_executable(giris.elf
  src/startup_at32f402_405.s
  src/main.c
  src/clock.c
  src/sk6812.c
  src/adc.c
  src/usb.c
  src/usb_descriptors.c
  src/console.c
  src/uid.c
  src/link.c
  src/peer.c
  src/split.c
  src/reset.c
  ${TUSB}/src/tusb.c
  ${TUSB}/src/common/tusb_fifo.c
  ${TUSB}/src/device/usbd.c
  ${TUSB}/src/class/hid/hid_device.c
  ${TUSB}/src/portable/synopsys/dwc2/dcd_dwc2.c
  ${TUSB}/src/portable/synopsys/dwc2/dwc2_common.c
  ${CMSIS}/device_support/system_at32f402_405.c
  ${DRIVERS}/src/at32f402_405_crm.c
  ${DRIVERS}/src/at32f402_405_gpio.c
  ${DRIVERS}/src/at32f402_405_misc.c
  ${DRIVERS}/src/at32f402_405_pwc.c
  ${DRIVERS}/src/at32f402_405_flash.c
  ${DRIVERS}/src/at32f402_405_adc.c
  ${DRIVERS}/src/at32f402_405_dma.c
  ${DRIVERS}/src/at32f402_405_tmr.c
  ${DRIVERS}/src/at32f402_405_usart.c
  ${DRIVERS}/src/at32f402_405_crc.c
)

target_include_directories(giris.elf PRIVATE
  include
  ${CMSIS}/core_support
  ${CMSIS}/device_support
  ${DRIVERS}/inc
  ${TUSB}/src
)

target_compile_definitions(giris.elf PRIVATE
  ${PART}
  USE_STDPERIPH_DRIVER
  HEXT_VALUE=12000000U     # 12 MHz crystal — the OTG_HS PHY's only legal reference
  CFG_TUSB_MCU=OPT_MCU_AT32F402_405
)

target_compile_options(giris.elf PRIVATE
  ${MCU_FLAGS}
  $<$<CONFIG:Debug>:-Og -g3>
  $<$<CONFIG:Release>:-O2 -g>
  -ffunction-sections -fdata-sections
  -Wall -Wextra -Wdouble-promotion -Wstrict-prototypes -Wno-unused-parameter
)

target_link_options(giris.elf PRIVATE
  ${MCU_FLAGS}
  -T${CMAKE_SOURCE_DIR}/ld/at32f405rbt7.ld
  --specs=nano.specs --specs=nosys.specs
  -Wl,--gc-sections
  -Wl,-Map=${CMAKE_BINARY_DIR}/giris.map,--cref
  -Wl,--print-memory-usage
  -Wl,--no-warn-rwx-segments
)

# --- artefacts -----------------------------------------------------------
add_custom_command(TARGET giris.elf POST_BUILD
  COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:giris.elf> ${CMAKE_BINARY_DIR}/giris.bin
  COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:giris.elf> ${CMAKE_BINARY_DIR}/giris.hex
  COMMAND ${CMAKE_SIZE_UTIL} $<TARGET_FILE:giris.elf>
  COMMENT "Generating giris.bin / giris.hex"
)

# --- flash over the ROM DFU (USB FS port J2) -----------------------------
add_custom_target(flash
  COMMAND ${CMAKE_SOURCE_DIR}/tools/flash.sh ${CMAKE_BINARY_DIR}/giris.bin
  DEPENDS giris.elf
  USES_TERMINAL
  COMMENT "Flashing via dfu-util"
)
