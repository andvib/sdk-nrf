# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(lc3_sd_card)

target_sources(app PRIVATE src/main.c)
target_sources(app PRIVATE ${ZEPHYR_NRF_MODULE_DIR}/applications/nrf5340_audio/src/modules/lc3_file.c)
target_sources(app PRIVATE ${ZEPHYR_NRF_MODULE_DIR}/applications/nrf5340_audio/src/modules/sd_card.c)
target_include_directories(app PRIVATE ${ZEPHYR_NRF_MODULE_DIR}/applications/nrf5340_audio/src)
