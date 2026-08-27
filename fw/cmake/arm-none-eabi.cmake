# Bare-metal ARM toolchain for the Giris firmware (AT32F405RBT7-7, Cortex-M4F).

set(CMAKE_SYSTEM_NAME       Generic)
set(CMAKE_SYSTEM_PROCESSOR  arm)

# Don't try to link a test executable during compiler detection — there is no libc startup here.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

find_program(ARM_CC  arm-none-eabi-gcc     REQUIRED)
find_program(ARM_CXX arm-none-eabi-g++     REQUIRED)
find_program(ARM_OBJCOPY arm-none-eabi-objcopy REQUIRED)
find_program(ARM_SIZE    arm-none-eabi-size    REQUIRED)

set(CMAKE_C_COMPILER   ${ARM_CC})
set(CMAKE_CXX_COMPILER ${ARM_CXX})
set(CMAKE_ASM_COMPILER ${ARM_CC})
set(CMAKE_OBJCOPY      ${ARM_OBJCOPY} CACHE FILEPATH "objcopy")
set(CMAKE_SIZE_UTIL    ${ARM_SIZE}    CACHE FILEPATH "size")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
