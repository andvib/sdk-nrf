#include "lc3_file.h"

#include "sd_card.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(lc3_file, 3);

int lc3_file_init(void)
{
	int err;

	err = sd_card_init();
	if (err) {
		LOG_ERR("Failed to initialize SD card: %d", err);
		return err;
	}

	return 0;
}