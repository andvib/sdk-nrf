#
# Copyright (c) 2024 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_lc3_file)

target_sources(app
	PRIVATE
	src/main.c
	${ZEPHYR_NRF_MODULE_DIR}/applications/nrf5340_audio/src/modules/lc3_file.c
	)

target_include_directories(app
	PRIVATE ${ZEPHYR_NRF_MODULE_DIR}/applications/nrf5340_audio/src)
