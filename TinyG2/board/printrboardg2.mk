# ----------------------------------------------------------------------------
# This file is part of the Synthetos G2 project.


# To compile:
#   make BOARD=pboard-a

# You can also choose a CONFIG from boards.mk:
#   make CONFIG=PrintrbotPlus BOARD=pboard-a


##########
# BOARDs for use directly from the make command line (with default settings) or by CONFIGs.

ifeq ("$(BOARD)","printrboardG2v3")
    BASE_BOARD=printrboardg2
    DEVICE_DEFINES += MOTATE_BOARD="printrboardG2v3"
    DEVICE_DEFINES += SETTINGS_FILE=${SETTINGS_FILE}
endif

##########
# The general pboard-a BASE_BOARD.

ifeq ("$(BASE_BOARD)","printrboardg2")
    _BOARD_FOUND = 1

    DEVICE_DEFINES += MOTATE_CONFIG_HAS_USBSERIAL=1

    FIRST_LINK_SOURCES += $(wildcard ${MOTATE_PATH}/Atmel_sam_common/*.cpp) $(wildcard ${MOTATE_PATH}/Atmel_sam3x/*.cpp)

    CHIP = SAM3X8C
    export CHIP
    CHIP_LOWERCASE = sam3x8c

    BOARD_PATH = ./board/pboard
    SOURCE_DIRS += ${BOARD_PATH} device/step_dir_driver

    PLATFORM_BASE = ${MOTATE_PATH}/platform/atmel_sam
    include $(PLATFORM_BASE).mk
endif
