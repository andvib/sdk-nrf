/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef __SAMPLE_RATE_CONVERTER__
#define __SAMPLE_RATE_CONVERTER__

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <dsp/filtering_functions.h>

/*
 * Must be larger than:
 * 	- decimator: numTaps + blockSize - 1 (529 for 50 taps)
 * 	- interpolator: (numTaps/L) + blockSize - 1 (504 for 50 taps)
 */
#define STATE_BUFFER_SIZE 600

enum sample_rate_converter_filter {
	SAMPLE_RATE_FILTER_SIMPLE,
	SAMPLE_RATE_FILTER_SMALL,
};

enum sample_rate_converter_direction {
	SAMPLE_RATE_DIRECTION_UP,
	SAMPLE_RATE_DIRECTION_DOWN,
};

struct buf_ctx {
	uint16_t bytes[10];
	size_t bytes_in_buf;
};

struct sample_rate_converter_ctx {
	int input_rate;
	int output_rate;
	enum sample_rate_converter_direction conversion_dir;
	enum sample_rate_converter_filter filter;
	q15_t state_buf[STATE_BUFFER_SIZE];
	struct buf_ctx input_buf;
	struct buf_ctx output_buf;
	union {
		arm_fir_interpolate_instance_q15 fir_interpolate;
		arm_fir_decimate_instance_q15 fir_decimate;
	};
};

int sample_rate_converter_process(struct sample_rate_converter_ctx *ctx,
				  enum sample_rate_converter_filter filter, void *input,
				  size_t input_size, uint32_t input_sample_rate, void *output,
				  size_t *output_size, uint32_t output_sample_rate);

#endif /* __ SAMPLE_RATE_CONVERTER__ */
