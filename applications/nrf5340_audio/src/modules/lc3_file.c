#include "lc3_file.h"

#include "sd_card.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(lc3_file, 3);

int lc3_file_frame_get(struct lc3_file_ctx *file, uint8_t *buffer, size_t buffer_size)
{
	int err;

	/* Read frame header */
	uint16_t frame_header;
	size_t frame_header_size = sizeof(frame_header);

	err = sd_card_read((char *)&frame_header, &frame_header_size, &file->file_object);
	if (err) {
		LOG_ERR("Failed to read frame header: %d", err);
		return err;
	}

	if (frame_header_size == 0) {
		LOG_ERR("No more frames to read");
		return -ENODATA;
	}

	if (frame_header == 0) {
		LOG_ERR("No more frames to read");
		return -ENODATA;
	}

	LOG_ERR("Frame header is %d", frame_header);

	/* Read frame data */
	size_t frame_size = frame_header;
	err = sd_card_read((char *)buffer, &frame_size, &file->file_object);
	if (err) {
		LOG_ERR("Failed to read frame data: %d", err);
		return err;
	}

	LOG_ERR("Frame size is %d", frame_size);

	return 0;
}

int lc3_file_open(struct lc3_file_ctx *file, const char *file_name)
{
	int err;
	size_t size = sizeof(file->lc3_header);

	err = sd_card_open(file_name, &file->file_object);
	if (err) {
		LOG_ERR("Failed to open file: %d", err);
		return err;
	}

	/* Read LC3 header and store in struct */
	err = sd_card_read((char *)&file->lc3_header, &size, &file->file_object);
	if (err) {
		LOG_ERR("Failer to read the LC3 header: %d", err);
		return err;
	}

	/* Get total number of samples */
	file->number_of_samples =
		(file->lc3_header.signal_len_msb << 16) + file->lc3_header.signal_len_lsb;
	LOG_ERR("NUM SAMPLES %d", file->number_of_samples);

	/* Calculate frame size */
	LOG_ERR("SAMPLE RATE: %d", file->lc3_header.sample_rate * 100);
	LOG_ERR("BIT RATE: %d", file->lc3_header.bit_rate * 100);
	LOG_ERR("FRAME DURATION: %d", file->lc3_header.frame_duration / 100);

	return 0;
}

int lc3_file_init(void)
{
	int err;

	LOG_ERR("calling sd_card_init\n");
	err = sd_card_init();
	if (err) {
		LOG_ERR("Failed to initialize SD card: %d", err);
		return err;
	}

	return 0;
}