/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "bluetooth/bt_stream/le_audio.h"

/**
 * - Function pointer for sending buffer
 */
void (*lc3_streamer_send_t)(struct le_audio_encoded_audio *frame);

/**
 * - Starts the streamer thread
 */
int lc3_streamer_start(void);

/**
 * - Registers a channel for streaming
 *    - Filename
 *    - Buffer index?
 */
int lc3_streamer_register_channel(void);

/**
 * - Sets frame duration
 * - Sets number of channels
 * - Allocates needed memory for buffer
 * - Registers callback for sending buffer
 */
int lc3_streamer_init(void);