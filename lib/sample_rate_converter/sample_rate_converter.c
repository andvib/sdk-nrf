/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <errno.h>
#include "sample_rate_converter.h"
#include "sample_rate_converter_filter.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(sample_rate_converter, 3);

#define BLOCK_SIZE 480

static uint16_t internal_input_buf[BLOCK_SIZE];

static int validate_sample_rates(uint32_t input_sample_rate, uint32_t output_sample_rate)
{
	if (input_sample_rate > output_sample_rate) {
		/* Downsampling */
		if (input_sample_rate != 48000) {
			LOG_ERR("Invalid input sample rate for downsampling %d", input_sample_rate);
			return -EINVAL;
		}

		if ((output_sample_rate != 24000) && (output_sample_rate != 16000)) {
			LOG_ERR("Invalid output sample rate for downsampling %d",
				output_sample_rate);
			return -EINVAL;
		}
	}

	if (input_sample_rate < output_sample_rate) {
		/* Upsampling */
		if (output_sample_rate != 48000) {
			LOG_ERR("Invalid output sample rate for downsampling: %d",
				output_sample_rate);
			return -EINVAL;
		}

		if ((input_sample_rate != 24000) && (input_sample_rate != 16000)) {
			LOG_ERR("Invalid input sample rate for downsampling: %d",
				input_sample_rate);
			return -EINVAL;
		}
	}
	return 0;
}

static int sample_rate_converter_init(struct sample_rate_converter_ctx *ctx, int input_sample_rate,
				      int output_sample_rate,
				      enum sample_rate_converter_filter filter)
{
	int ret;
	arm_status err;
	size_t filter_size;
	q15_t *filter_coeffs;

	// Validate input and output samplerate
	ret = validate_sample_rates(input_sample_rate, output_sample_rate);
	if (ret) {
		LOG_ERR("Invalid sample rate given (%d)", ret);
		return ret;
	}

	ctx->input_rate = input_sample_rate;
	ctx->output_rate = output_sample_rate;

	// Calculate conversion direction
	if (input_sample_rate > output_sample_rate) {
		ctx->conversion_dir = SAMPLE_RATE_DIRECTION_DOWN;
	} else {
		ctx->conversion_dir = SAMPLE_RATE_DIRECTION_UP;
	}

	// Select the filter
	ctx->filter = filter;
	uint8_t conversion_ratio = (ctx->conversion_dir == SAMPLE_RATE_DIRECTION_DOWN)
					   ? (input_sample_rate / output_sample_rate)
					   : (output_sample_rate / input_sample_rate);
	get_filter(filter, conversion_ratio, &filter_coeffs, &filter_size);

	// Initialize interpolator/decimator
	switch (ctx->conversion_dir) {
	case SAMPLE_RATE_DIRECTION_UP:
		// Interpolator
		err = arm_fir_interpolate_init_q15(&ctx->fir_interpolate, conversion_ratio,
						   filter_size, filter_coeffs, ctx->state_buf,
						   BLOCK_SIZE);
		if (err != ARM_MATH_SUCCESS) {
			LOG_ERR("Failed to initialize interpolator (%d)", err);
			return err;
		}
		break;
	case SAMPLE_RATE_DIRECTION_DOWN:
		// Decimator
		err = arm_fir_decimate_init_q15(&ctx->fir_decimate, filter_size, conversion_ratio,
						filter_coeffs, ctx->state_buf, BLOCK_SIZE);
		if (err != ARM_MATH_SUCCESS) {
			LOG_ERR("Failed to initialize decimator (%d)", err);
			return err;
		}
		break;
	}

	if ((ctx->conversion_dir == SAMPLE_RATE_DIRECTION_DOWN) || (conversion_ratio != 3)) {
		ctx->input_buf.bytes_in_buf = 0;
		ctx->output_buf.bytes_in_buf = 0;
	} else {
		memset(ctx->input_buf.bytes, 0, 2 * sizeof(uint16_t));
		ctx->input_buf.bytes_in_buf = 2;
		ctx->output_buf.bytes_in_buf = 0;
	}

	return 0;
}

int sample_rate_converter_process(struct sample_rate_converter_ctx *ctx,
				  enum sample_rate_converter_filter filter, void *input,
				  size_t input_size, uint32_t input_sample_rate, void *output,
				  size_t *output_size, uint32_t output_sample_rate)
{
	int ret;
	uint8_t conversion_ratio;
	size_t samples_in = input_size / 2;
	size_t samples_to_process;
	q15_t *read_ptr;
	q15_t *write_ptr;

	if (input_sample_rate == output_sample_rate) {
		LOG_ERR("Input and output sample rates are equal");
		return -EINVAL;
	}

	// Validate bit depth is 16

	// Validate structure is initialized and unchanged
	if ((ctx->input_rate != input_sample_rate) || (ctx->output_rate != output_sample_rate) ||
	    (ctx->filter != filter)) {
		LOG_DBG("State has changed, re-initializing filter");
		ret = sample_rate_converter_init(ctx, input_sample_rate, output_sample_rate,
						 filter);
		if (ret) {
			LOG_ERR("Failed to initialize converter (%d)", ret);
			return ret;
		}
	}

	conversion_ratio = (ctx->conversion_dir == SAMPLE_RATE_DIRECTION_DOWN)
				   ? (input_sample_rate / output_sample_rate)
				   : (output_sample_rate / input_sample_rate);

	// Prepare input buffer
	//	- If overflow is not needed -> only update pointer
	// 	- Initialize buffer for full frame
	//	- Place extra bytes at start of array
	//	- Copy input bytes after extra bytes
	//	- Calculate how many bytes can be used in interpolation/decimation
	//	- Move extra bytes to buffer
	if ((samples_in % conversion_ratio) == 0) {
		/* Do nothing */
		samples_to_process = samples_in;
	} else if (((samples_in + ctx->input_buf.bytes_in_buf) % conversion_ratio) == 0) {
		/* Use extra bytes */
		samples_to_process =
			samples_in + (conversion_ratio - (samples_in % conversion_ratio));
	} else {
		/* Store extra bytes */
		size_t extra_bytes = (samples_in % conversion_ratio);
		samples_to_process = samples_in - extra_bytes;
	}

	if (ctx->input_buf.bytes_in_buf > 0) {
		read_ptr = internal_input_buf;
		memcpy(internal_input_buf, ctx->input_buf.bytes,
		       ctx->input_buf.bytes_in_buf * sizeof(uint16_t));
		memcpy(internal_input_buf + ctx->input_buf.bytes_in_buf, input,
		       samples_in * sizeof(uint16_t));
	} else {
		read_ptr = input;
	}
	// Prepare output buffer
	//	- If overflow is not needed -> only update pointer
	//	- Initialize buffer for full output frame
	//	- Place extra bytes at start of array
	//	- Update pointer to after extra bytes
	write_ptr = output;

	//  Perform interpolation/decimation
	switch (ctx->conversion_dir) {
	case SAMPLE_RATE_DIRECTION_UP:
		arm_fir_interpolate_q15(&ctx->fir_interpolate, read_ptr, write_ptr,
					samples_to_process);
		break;
	case SAMPLE_RATE_DIRECTION_DOWN:
		arm_fir_decimate_q15(&ctx->fir_decimate, read_ptr, write_ptr, samples_to_process);
		break;
	}

	// Store overflow input bytes (if any)
	if (samples_to_process < samples_in) {
		// Overflow bytes in input buffer
		size_t number_overflow_samples = samples_in - samples_to_process;
		ctx->input_buf.bytes_in_buf += number_overflow_samples;
		memcpy(ctx->input_buf.bytes, read_ptr + samples_to_process,
		       ctx->input_buf.bytes_in_buf * sizeof(uint16_t));
	} else if ((ctx->input_buf.bytes_in_buf) &&
		   (((samples_in + ctx->input_buf.bytes_in_buf) % conversion_ratio) == 0)) {
		// Overflow bytes has been used
		uint8_t overflow_bytes_used = conversion_ratio - (samples_in % conversion_ratio);
		ctx->input_buf.bytes_in_buf -= overflow_bytes_used;
	}

	// Copy output bytes to output buffer (if needed)
	if (output_sample_rate == 48000) {
		*output_size = 960;
	} else if (output_sample_rate == 24000) {
		*output_size = 480;
	} else {
		*output_size = 320;
	}

	return 0;
}
