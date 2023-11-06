/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "sample_rate_converter.h"
#include "sample_rate_converter_filter.h"

// #define AWARE_FILTER_48KHZ_16KHZ_SMALL_TAP_NUM (33)
static q15_t filter_48khz_16khz_small[] = {
	0xfff7, 0xfff9, 0x0021, 0x0052, 0x0022, 0xff66, 0xfed7, 0xffac, 0x01df, 0x0319, 0x0091,
	0xfb02, 0xf847, 0xff3a, 0x1076, 0x234f, 0x2b82, 0x234f, 0x1076, 0xff3a, 0xf847, 0xfb02,
	0x0091, 0x0319, 0x01df, 0xffac, 0xfed7, 0xff66, 0x0022, 0x0052, 0x0021, 0xfff9, 0xfff7};

static q15_t filter_48khz_24kz_simple[] = {16383, 16383};

static q15_t filter_48khz_16kz_simple[] = {10922, 10922, 10923};

void get_filter(enum sample_rate_converter_filter filter_type, uint8_t conversion_ratio,
		q15_t **filter_ptr, size_t *filter_size)
{
	switch (filter_type) {
	case SAMPLE_RATE_FILTER_SIMPLE:
		if (conversion_ratio == 2) {
			*filter_ptr = filter_48khz_24kz_simple;
			*filter_size = ARRAY_SIZE(filter_48khz_24kz_simple);
		} else if (conversion_ratio == 3) {
			*filter_ptr = filter_48khz_16kz_simple;
			*filter_size = ARRAY_SIZE(filter_48khz_16kz_simple);
		}
		break;
	case SAMPLE_RATE_FILTER_SMALL:
		if (conversion_ratio == 3) {
			*filter_ptr = filter_48khz_16khz_small;
			*filter_size = ARRAY_SIZE(filter_48khz_16khz_small);
		}
		break;
	}
}
