# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(lc3_stream)

target_sources(app PRIVATE src/main.c)
target_sources(app PRIVATE ${ZEPHYR_NRF_MODULE_DIR}/applications/nrf5340_audio/src/modules/lc3_file.c)
target_sources(app PRIVATE ${ZEPHYR_NRF_MODULE_DIR}/applications/nrf5340_audio/src/modules/sd_card.c)
target_sources(app PRIVATE ${ZEPHYR_NRF_MODULE_DIR}/applications/nrf5340_audio/src/modules/lc3_streamer.c)
target_include_directories(app PRIVATE ${ZEPHYR_NRF_MODULE_DIR}/applications/nrf5340_audio/src)

target_compile_definitions(app PRIVATE CONFIG_MODULE_SD_CARD_LOG_LEVEL=3)
target_compile_definitions(app PRIVATE CONFIG_MODULE_SD_CARD_LC3_FILE_LOG_LEVEL=3)
target_compile_definitions(app PRIVATE CONFIG_SD_CARD_LC3_STREAMER_STACK_SIZE=1500)
target_compile_definitions(app PRIVATE CONFIG_SD_CARD_LC3_STREAMER_THREAD_PRIORITY=-11)
target_compile_definitions(app PRIVATE CONFIG_SD_CARD_LC3_STREAMER_MAX_NUM_STREAMS=3)
target_compile_definitions(app PRIVATE CONFIG_SD_CARD_LC3_STREAMER_MAX_FRAME_SIZE=240)
#target_compile_definitions(app PRIVATE CONFIG_FS_FATFS_MAX_LFN=40)