/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <stddef.h>
#include <stdint.h>

/**
 * - Reads the next frame from the file
 * - Must have logic for file empty
 */
int lc3_file_frame_get(struct lc3_file_ctx *file, uint8_t *buffer, size_t buffer_size);

/**
 * - Open the file
 * - Read the LC3 header
 * - Calculate frame sizes
 */
int lc3_file_open(struct lc3_file_ctx *file, const char *file_name);

/**
 * - Initialize SD card driver
 * - Initialize file system
 */
int lc3_file_init(void);