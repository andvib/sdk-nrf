#include <stdio.h>

#include <zephyr/kernel.h>
#include <nrfx_clock.h>
#include <modules/lc3_file.h>
#include <modules/lc3_streamer.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(lc3_sample, 3);

int main(void)
{
	LOG_ERR("Hello World! %s\n", CONFIG_BOARD_TARGET);

	int err;

	err = nrfx_clock_divider_set(NRF_CLOCK_DOMAIN_HFCLK, NRF_CLOCK_HFCLK_DIV_1);
	err -= NRFX_ERROR_BASE_NUM;
	if (err) {
		return err;
	}

	err = lc3_streamer_init();
	if (err) {
		LOG_ERR("Failed to initialize LC3 streamer %d\n", err);
		return err;
	}

	ssize_t idx_1;
	ssize_t idx_2;

	err = lc3_streamer_register_stream("FILE1.LC3", &idx_1, true);
	if (err) {
		LOG_ERR("Failed to register stream: %d\n", err);
		return err;
	}

	err = lc3_streamer_register_stream("FILE2.LC3", &idx_2, true);
	if (err) {
		LOG_ERR("Failed to register stream: %d\n", err);
		return err;
	}

	int counter = 0;

	uint8_t *data_ptr;
	err = lc3_streamer_next_frame_get(idx_1, &data_ptr);
	if (err) {
		LOG_ERR("file 1 :: Failed to get next frame %d\n", err);
		return err;
	}

	k_msleep(20);

	while (true) {
		uint8_t *data_ptr_1;
		uint8_t *data_ptr_2;

		err = lc3_streamer_next_frame_get(idx_1, &data_ptr_1);
		if (err) {
			LOG_ERR("file 1 :: Failed to get next frame %d\n", err);
			return err;
		}

		k_msleep(5);

		err = lc3_streamer_next_frame_get(idx_2, &data_ptr_2);
		if (err) {
			LOG_ERR("file 2 :: Failed to get next frame %d\n", err);
			return err;
		}

		// LOG_ERR("[File 1] :: Frame %d :: 0x%2x : 0x%2x", counter, data_ptr_1[0],
		//	data_ptr_1[1]);
		// LOG_ERR("[File 2] :: Frame %d :: 0x%2x : 0x%2x", counter, data_ptr_2[0],
		//	data_ptr_2[1]);

		counter++;

		k_msleep(5);
	}

	LOG_ERR("End of sample\n");

	return 0;
}
