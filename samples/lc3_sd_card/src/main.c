#include <stdio.h>

#include <zephyr/kernel.h>

#include <modules/lc3_file.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(lc3_sample, 3);

int main(void)
{
	LOG_ERR("Hello World! %s\n", CONFIG_BOARD_TARGET);

	int err;

	LOG_ERR("lc3 file init\n");
	err = lc3_file_init();
	if (err) {
		LOG_ERR("Failed to initialize LC3 file system %d\n", err);
		return err;
	}

	struct lc3_file_ctx file;

	err = lc3_file_open(&file, "FILE1.LC3");
	if (err) {
		LOG_ERR("Failed to open file: %d\n", err);
		return err;
	}

	err = 0;
	int count = 0;
	while (err == 0) {
		uint8_t buffer[1024];
		printk("getting frame %d\n", count++);
		err = lc3_file_frame_get(&file, buffer, sizeof(buffer));
		if (err) {
			LOG_ERR("Failed to get frame: %d\n", err);
			break;
		}
		k_msleep(500);
	}

	LOG_ERR("End of sample\n");

	return 0;
}