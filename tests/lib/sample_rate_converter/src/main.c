/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/ztest.h>
#include <zephyr/tc_util.h>
#include <sample_rate_converter.h>

uint16_t input_buf[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
uint16_t output_buf[10];

ZTEST(suite_sample_rate_converter, test_init_valid_decimate_24khz)
{
	int ret;
	size_t output_size;
	struct sample_rate_converter_ctx conv_ctx;

	size_t num_samples = 12;
	uint16_t input_bytes[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
	uint16_t output_bytes[num_samples];

	uint32_t input_sample_rate = 48000;
	uint32_t output_sample_rate = 24000;
	enum sample_rate_converter_filter filter = SAMPLE_RATE_FILTER_SIMPLE;

	ret = sample_rate_converter_process(&conv_ctx, filter, input_bytes, num_samples * 2,
					    input_sample_rate, output_bytes, &output_size,
					    output_sample_rate);

	zassert_equal(ret, 0, "Init failed");
	zassert_equal(conv_ctx.input_rate, input_sample_rate, "Input sample rate not as expected");
	zassert_equal(conv_ctx.output_rate, output_sample_rate,
		      "Output sample rate not as expected");
	zassert_equal(conv_ctx.conversion_dir, SAMPLE_RATE_DIRECTION_DOWN,
		      "Conversion direction not as expected");
	zassert_equal(conv_ctx.filter, filter, "Filter set incorrectly");
	zassert_equal(conv_ctx.input_buf.bytes_in_buf, 0, "Bytes in input buffer not as expected");
	zassert_equal(conv_ctx.output_buf.bytes_in_buf, 0,
		      "Bytes in output buffer not as expected");
}

ZTEST(suite_sample_rate_converter, test_init_valid_decimate_16khz)
{
	int ret;
	size_t output_size;
	struct sample_rate_converter_ctx conv_ctx;

	size_t num_samples = 12;
	uint16_t input_bytes[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
	uint16_t output_bytes[num_samples];

	uint32_t input_sample_rate = 48000;
	uint32_t output_sample_rate = 16000;
	enum sample_rate_converter_filter filter = SAMPLE_RATE_FILTER_SIMPLE;

	ret = sample_rate_converter_process(&conv_ctx, filter, input_bytes, num_samples * 2,
					    input_sample_rate, output_bytes, &output_size,
					    output_sample_rate);

	zassert_equal(ret, 0, "Init failed");
	zassert_equal(conv_ctx.input_rate, input_sample_rate, "Input sample rate not as expected");
	zassert_equal(conv_ctx.output_rate, output_sample_rate,
		      "Output sample rate not as expected");
	zassert_equal(conv_ctx.conversion_dir, SAMPLE_RATE_DIRECTION_DOWN,
		      "Conversion direction not as expected");
	zassert_equal(conv_ctx.filter, filter, "Filter set incorrectly");
	zassert_equal(conv_ctx.input_buf.bytes_in_buf, 0, "Bytes in input buffer not as expected");
	zassert_equal(conv_ctx.output_buf.bytes_in_buf, 0,
		      "Bytes in output buffer not as expected");
}

ZTEST(suite_sample_rate_converter, test_init_valid_interpolate_24khz)
{
	int ret;
	size_t output_size;
	struct sample_rate_converter_ctx conv_ctx;

	size_t num_samples = 6;
	uint16_t input_bytes[] = {1, 2, 3, 4, 5, 6};
	uint16_t output_bytes[num_samples * 2];

	uint32_t input_sample_rate = 24000;
	uint32_t output_sample_rate = 48000;
	enum sample_rate_converter_filter filter = SAMPLE_RATE_FILTER_SIMPLE;

	ret = sample_rate_converter_process(&conv_ctx, filter, input_bytes, num_samples * 2,
					    input_sample_rate, output_bytes, &output_size,
					    output_sample_rate);

	zassert_equal(ret, 0, "Init failed");
	zassert_equal(conv_ctx.input_rate, input_sample_rate, "Input sample rate not as expected");
	zassert_equal(conv_ctx.output_rate, output_sample_rate,
		      "Output sample rate not as expected");
	zassert_equal(conv_ctx.conversion_dir, SAMPLE_RATE_DIRECTION_UP,
		      "Conversion direction not as expected");
	zassert_equal(conv_ctx.filter, filter, "Filter set incorrectly");
	zassert_equal(conv_ctx.input_buf.bytes_in_buf, 0, "Bytes in input buffer not as expected");
	zassert_equal(conv_ctx.output_buf.bytes_in_buf, 0,
		      "Bytes in output buffer not as expected");
}

ZTEST(suite_sample_rate_converter, test_init_valid_interpolate_16khz)
{
	int ret;
	size_t output_size;
	struct sample_rate_converter_ctx conv_ctx;

	size_t num_samples = 4;
	uint16_t input_one[] = {1, 2, 3, 4};
	uint16_t input_two[] = {5, 6, 7, 8};
	uint16_t input_three[] = {9, 10, 11, 12};
	uint16_t input_four[] = {13, 14, 15, 16};

	uint16_t output_bytes[18];

	uint32_t input_sample_rate = 16000;
	uint32_t output_sample_rate = 48000;
	enum sample_rate_converter_filter filter = SAMPLE_RATE_FILTER_SIMPLE;

	/* First run */
	ret = sample_rate_converter_process(&conv_ctx, filter, input_one, num_samples * 2,
					    input_sample_rate, output_bytes, &output_size,
					    output_sample_rate);

	zassert_equal(ret, 0, "Init failed");
	zassert_equal(conv_ctx.input_rate, input_sample_rate,
		      "Input sample rate not as expected %d", conv_ctx.input_rate);
	zassert_equal(conv_ctx.output_rate, output_sample_rate,
		      "Output sample rate not as expected");
	zassert_equal(conv_ctx.conversion_dir, SAMPLE_RATE_DIRECTION_UP,
		      "Conversion direction not as expected");
	zassert_equal(conv_ctx.filter, filter, "Filter set incorrectly");
	zassert_equal(conv_ctx.input_buf.bytes_in_buf, 0,
		      "Bytes in input buffer not as expected %d", conv_ctx.input_buf.bytes_in_buf);

	uint16_t expected_input_buf_one[] = {0};
	zassert_mem_equal(conv_ctx.input_buf.bytes, expected_input_buf_one, 0 * sizeof(uint16_t));
	zassert_equal(conv_ctx.output_buf.bytes_in_buf, 0,
		      "Bytes in output buffer not as expected");

	/* Second run */
	ret = sample_rate_converter_process(&conv_ctx, filter, input_two, num_samples * 2,
					    input_sample_rate, output_bytes, &output_size,
					    output_sample_rate);

	zassert_equal(ret, 0, "Init failed");
	zassert_equal(conv_ctx.input_rate, input_sample_rate,
		      "Input sample rate not as expected %d", conv_ctx.input_rate);
	zassert_equal(conv_ctx.output_rate, output_sample_rate,
		      "Output sample rate not as expected");
	zassert_equal(conv_ctx.conversion_dir, SAMPLE_RATE_DIRECTION_UP,
		      "Conversion direction not as expected");
	zassert_equal(conv_ctx.filter, filter, "Filter set incorrectly");
	zassert_equal(conv_ctx.input_buf.bytes_in_buf, 1,
		      "Bytes in input buffer not as expected %d", conv_ctx.input_buf.bytes_in_buf);

	uint16_t expected_input_buf_two[] = {8};
	zassert_mem_equal(conv_ctx.input_buf.bytes, expected_input_buf_two, 1 * sizeof(uint16_t));
	zassert_equal(conv_ctx.output_buf.bytes_in_buf, 0,
		      "Bytes in output buffer not as expected");

	/* Third run */
	ret = sample_rate_converter_process(&conv_ctx, filter, input_three, num_samples * 2,
					    input_sample_rate, output_bytes, &output_size,
					    output_sample_rate);

	zassert_equal(ret, 0, "Init failed");
	zassert_equal(conv_ctx.input_rate, input_sample_rate,
		      "Input sample rate not as expected %d", conv_ctx.input_rate);
	zassert_equal(conv_ctx.output_rate, output_sample_rate,
		      "Output sample rate not as expected");
	zassert_equal(conv_ctx.conversion_dir, SAMPLE_RATE_DIRECTION_UP,
		      "Conversion direction not as expected");
	zassert_equal(conv_ctx.filter, filter, "Filter set incorrectly");
	zassert_equal(conv_ctx.input_buf.bytes_in_buf, 2,
		      "Bytes in input buffer not as expected %d", conv_ctx.input_buf.bytes_in_buf);

	uint16_t expected_input_buf_three[] = {11, 12};
	zassert_mem_equal(conv_ctx.input_buf.bytes, expected_input_buf_three, 2 * sizeof(uint16_t));
	zassert_equal(conv_ctx.output_buf.bytes_in_buf, 0,
		      "Bytes in output buffer not as expected");

	/* Fourth run */
	ret = sample_rate_converter_process(&conv_ctx, filter, input_four, num_samples * 2,
					    input_sample_rate, output_bytes, &output_size,
					    output_sample_rate);

	zassert_equal(ret, 0, "Init failed");
	zassert_equal(conv_ctx.input_rate, input_sample_rate,
		      "Input sample rate not as expected %d", conv_ctx.input_rate);
	zassert_equal(conv_ctx.output_rate, output_sample_rate,
		      "Output sample rate not as expected");
	zassert_equal(conv_ctx.conversion_dir, SAMPLE_RATE_DIRECTION_UP,
		      "Conversion direction not as expected");
	zassert_equal(conv_ctx.filter, filter, "Filter set incorrectly");
	zassert_equal(conv_ctx.input_buf.bytes_in_buf, 0,
		      "Bytes in input buffer not as expected %d", conv_ctx.input_buf.bytes_in_buf);

	uint16_t expected_input_buf_four[] = {0};
	zassert_mem_equal(conv_ctx.input_buf.bytes, expected_input_buf_four, 0 * sizeof(uint16_t));
	zassert_equal(conv_ctx.output_buf.bytes_in_buf, 0,
		      "Bytes in output buffer not as expected");
}

ZTEST(suite_sample_rate_converter, test_init_valid_sample_rates_changed)
{
	int ret;
	size_t output_size;
	struct sample_rate_converter_ctx conv_ctx;

	uint32_t original_input_rate = 48000;
	uint32_t original_output_rate = 16000;
	enum sample_rate_converter_direction original_direction = SAMPLE_RATE_DIRECTION_DOWN;

	uint32_t new_input_rate = 16000;
	uint32_t new_output_rate = 48000;
	enum sample_rate_converter_direction new_direction = SAMPLE_RATE_DIRECTION_UP;

	conv_ctx.input_rate = original_input_rate;
	conv_ctx.output_rate = original_output_rate;
	conv_ctx.conversion_dir = original_direction;

	ret = sample_rate_converter_process(&conv_ctx, SAMPLE_RATE_FILTER_SIMPLE, input_buf, 10,
					    new_input_rate, output_buf, &output_size,
					    new_output_rate);

	zassert_equal(ret, 0, "Init failed");
	zassert_equal(conv_ctx.input_rate, new_input_rate, "Input sample rate not as expected");
	zassert_equal(conv_ctx.output_rate, new_output_rate, "Output sample rate not as expected");
	zassert_equal(conv_ctx.conversion_dir, new_direction,
		      "Conversion direction not as expected");
}

ZTEST(suite_sample_rate_converter, test_init_invalid_sample_rates)
{
	int ret;
	size_t output_size;
	struct sample_rate_converter_ctx conv_ctx;

	uint32_t input_rate;
	uint32_t output_rate;

	// Downsampling from NOT 48000
	input_rate = 24000;
	output_rate = 16000;

	ret = sample_rate_converter_process(&conv_ctx, SAMPLE_RATE_FILTER_SIMPLE, input_buf, 10,
					    input_rate, output_buf, &output_size, output_rate);

	zassert_equal(ret, -EINVAL);

	// Downsampling to invalid rate
	input_rate = 48000;
	output_rate = 20000;

	ret = sample_rate_converter_process(&conv_ctx, SAMPLE_RATE_FILTER_SIMPLE, input_buf, 10,
					    input_rate, output_buf, &output_size, output_rate);

	zassert_equal(ret, -EINVAL);

	// Upsampling to NOT 48000
	input_rate = 16000;
	output_rate = 24000;

	ret = sample_rate_converter_process(&conv_ctx, SAMPLE_RATE_FILTER_SIMPLE, input_buf, 10,
					    input_rate, output_buf, &output_size, output_rate);

	zassert_equal(ret, -EINVAL);

	// Upsampling to invalid rate
	input_rate = 24000;
	output_rate = 30000;

	ret = sample_rate_converter_process(&conv_ctx, SAMPLE_RATE_FILTER_SIMPLE, input_buf, 10,
					    input_rate, output_buf, &output_size, output_rate);

	zassert_equal(ret, -EINVAL);
}

ZTEST(suite_sample_rate_converter, test_init_invalid_sample_rates_equal)
{
	int ret;
	size_t output_size;
	struct sample_rate_converter_ctx conv_ctx;

	uint32_t sample_rate = 48000;

	ret = sample_rate_converter_process(&conv_ctx, SAMPLE_RATE_FILTER_SIMPLE, input_buf, 10,
					    sample_rate, output_buf, &output_size, sample_rate);

	zassert_equal(-EINVAL, ret, "sample_rate_converter_process did not fail");
}

ZTEST(suite_sample_rate_converter, test_init_valid_filter_changed)
{
	int ret;
	size_t output_size;
	struct sample_rate_converter_ctx conv_ctx;

	uint32_t input_sample_rate = 48000;
	uint32_t output_sample_rate = 24000;
	enum sample_rate_converter_direction original_direction = SAMPLE_RATE_DIRECTION_DOWN;
	enum sample_rate_converter_filter original_filter = SAMPLE_RATE_FILTER_SMALL;
	enum sample_rate_converter_filter new_filter = SAMPLE_RATE_FILTER_SIMPLE;

	conv_ctx.input_rate = input_sample_rate;
	conv_ctx.output_rate = output_sample_rate;
	conv_ctx.conversion_dir = original_direction;
	conv_ctx.filter = original_filter;

	ret = sample_rate_converter_process(&conv_ctx, new_filter, input_buf, 10, input_sample_rate,
					    output_buf, &output_size, output_sample_rate);

	zassert_equal(ret, 0, "Init failed");
	zassert_equal(conv_ctx.input_rate, input_sample_rate, "Input sample rate not as expected");
	zassert_equal(conv_ctx.output_rate, output_sample_rate,
		      "Output sample rate not as expected");
	zassert_equal(conv_ctx.conversion_dir, original_direction,
		      "Conversion direction not as expected");
	zassert_equal(conv_ctx.filter, new_filter, "Filter set incorrectly");
}

ZTEST(suite_sample_rate_converter, test_process_valid_decimate_24khz)
{
	int ret;
	size_t output_size;
	struct sample_rate_converter_ctx conv_ctx;

	size_t num_samples = 12;
	uint16_t input_bytes[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
	uint16_t output_bytes[num_samples];

	uint32_t input_sample_rate = 48000;
	uint32_t output_sample_rate = 24000;
	enum sample_rate_converter_filter filter = SAMPLE_RATE_FILTER_SIMPLE;

	ret = sample_rate_converter_process(&conv_ctx, filter, input_bytes, num_samples * 2,
					    input_sample_rate, output_bytes, &output_size,
					    output_sample_rate);

	zassert_equal(ret, 0, "Process call failed");

	zassert_equal(conv_ctx.input_buf.bytes_in_buf, 0, "Input buffer contains bytes");
}

ZTEST(suite_sample_rate_converter, test_process_valid_decimate_16khz)
{
	int ret;
	size_t output_size;
	struct sample_rate_converter_ctx conv_ctx;

	size_t num_samples = 12;
	uint16_t input_bytes[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
	uint16_t output_bytes[num_samples];

	uint32_t input_sample_rate = 48000;
	uint32_t output_sample_rate = 16000;
	enum sample_rate_converter_filter filter = SAMPLE_RATE_FILTER_SIMPLE;

	ret = sample_rate_converter_process(&conv_ctx, filter, input_bytes, num_samples * 2,
					    input_sample_rate, output_bytes, &output_size,
					    output_sample_rate);

	zassert_equal(ret, 0, "Process call failed");

	zassert_equal(conv_ctx.input_buf.bytes_in_buf, 0, "Input buffer contains bytes");
}

ZTEST(suite_sample_rate_converter, test_process_valid_interpolate_24khz)
{
	int ret;
	size_t output_size;
	struct sample_rate_converter_ctx conv_ctx;

	size_t num_samples = 6;
	uint16_t input_bytes[] = {1, 2, 3, 4, 5, 6};
	uint16_t output_bytes[num_samples];

	uint32_t input_sample_rate = 24000;
	uint32_t output_sample_rate = 48000;
	enum sample_rate_converter_filter filter = SAMPLE_RATE_FILTER_SIMPLE;

	ret = sample_rate_converter_process(&conv_ctx, filter, input_bytes, num_samples * 2,
					    input_sample_rate, output_bytes, &output_size,
					    output_sample_rate);

	zassert_equal(ret, 0, "Process call failed");

	zassert_equal(conv_ctx.input_buf.bytes_in_buf, 0, "Input buffer contains bytes");
}

ZTEST(suite_sample_rate_converter, test_process_valid_interpolate_16khz)
{
	int ret;
	size_t output_size;
	struct sample_rate_converter_ctx conv_ctx;

	size_t num_samples = 4;
	uint16_t input_bytes[] = {1, 2, 3, 4};
	uint16_t output_bytes[num_samples];

	uint32_t input_sample_rate = 16000;
	uint32_t output_sample_rate = 48000;
	enum sample_rate_converter_filter filter = SAMPLE_RATE_FILTER_SIMPLE;

	ret = sample_rate_converter_process(&conv_ctx, filter, input_bytes, num_samples * 2,
					    input_sample_rate, output_bytes, &output_size,
					    output_sample_rate);

	zassert_equal(ret, 0, "Process call failed");

	zassert_equal(conv_ctx.input_buf.bytes_in_buf, 0, "Input buffer contains bytes");

	// output buf should contain bytes here
}

ZTEST_SUITE(suite_sample_rate_converter, NULL, NULL, NULL, NULL, NULL);
