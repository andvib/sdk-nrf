/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/ztest.h>
#include <zephyr/fff.h>
#include <modules/lc3_file.h>

DEFINE_FFF_GLOBALS;

FAKE_VALUE_FUNC(int, sd_card_init);

static void test_setup(void *f)
{
	RESET_FAKE(sd_card_init);

	FFF_RESET_HISTORY();
}

ZTEST(lc3_file, test_lc3_file_init)
{
	int ret;

	ret = lc3_file_init();

	zassert_equal(0, ret, "lc3_file_init() should return 0");
	zassert_equal(1, sd_card_init_fake.call_count, "sd_card_init() should be called once");
}

ZTEST(lc3_file, test_lc3_file_init_invalid_sd_card)
{
	sd_card_init_fake.return_val = -EINVAL;

	int ret = lc3_file_init();

	zassert_equal(-EINVAL, ret, "lc3_file_init() should return an error");
	zassert_equal(1, sd_card_init_fake.call_count, "sd_card_init() should be called once");
}

ZTEST_SUITE(lc3_file, NULL, NULL, test_setup, NULL, NULL);
