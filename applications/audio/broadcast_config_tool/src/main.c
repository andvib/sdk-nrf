/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <stdlib.h>
#include <strings.h>
#include <ctype.h>
#include <zephyr/bluetooth/audio/audio.h>
#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/shell/shell.h>
#include <nrfx_clock.h>

#include "broadcast_config_tool.h"
#include "broadcast_source.h"
#include "zbus_common.h"
#include "macros_common.h"
#include "bt_mgmt.h"
#include "sd_card.h"
#include "lc3_streamer.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(main, CONFIG_MAIN_LOG_LEVEL);

ZBUS_CHAN_DECLARE(bt_mgmt_chan);
ZBUS_CHAN_DECLARE(sdu_ref_chan);
ZBUS_CHAN_DECLARE(le_audio_chan);
ZBUS_MSG_SUBSCRIBER_DEFINE(le_audio_evt_sub);

static struct k_thread le_audio_msg_sub_thread_data;
static k_tid_t le_audio_msg_sub_thread_id;
K_THREAD_STACK_DEFINE(le_audio_msg_sub_thread_stack, CONFIG_LE_AUDIO_MSG_SUB_STACK_SIZE);

struct bt_le_ext_adv *ext_adv;

/* Buffer for the UUIDs. */
#define EXT_ADV_UUID_BUF_SIZE (128)
NET_BUF_SIMPLE_DEFINE_STATIC(uuid_data0, EXT_ADV_UUID_BUF_SIZE);
NET_BUF_SIMPLE_DEFINE_STATIC(uuid_data1, EXT_ADV_UUID_BUF_SIZE);

/* Buffer for periodic advertising BASE data. */
#define BASE_DATA_BUF_SIZE (256)
NET_BUF_SIMPLE_DEFINE_STATIC(base_data0, BASE_DATA_BUF_SIZE);
NET_BUF_SIMPLE_DEFINE_STATIC(base_data1, BASE_DATA_BUF_SIZE);

/* Extended advertising buffer. */
static struct bt_data ext_adv_buf[CONFIG_BT_ISO_MAX_BIG][CONFIG_EXT_ADV_BUF_MAX];

/* Periodic advertising buffer. */
static struct bt_data per_adv_buf[CONFIG_BT_ISO_MAX_BIG];

/* Total size of the PBA buffer includes the 16-bit UUID, 8-bit features and the
 * meta data size.
 */
#define BROADCAST_SRC_PBA_BUF_SIZE                                                                 \
	(BROADCAST_SOURCE_PBA_HEADER_SIZE + CONFIG_BT_AUDIO_BROADCAST_PBA_METADATA_SIZE)

/* Number of metadata items that can be assigned. */
#define BROADCAST_SOURCE_PBA_METADATA_VACANT                                                       \
	(CONFIG_BT_AUDIO_BROADCAST_PBA_METADATA_SIZE / (sizeof(struct bt_data)))

/* Make sure pba_buf is large enough for a 16bit UUID and meta data
 * (any addition to pba_buf requires an increase of this value)
 */
uint8_t pba_data[CONFIG_BT_ISO_MAX_BIG][BROADCAST_SRC_PBA_BUF_SIZE];

/**
 * @brief	Broadcast source static extended advertising data.
 */
static struct broadcast_source_ext_adv_data ext_adv_data[] = {
	{.uuid_buf = &uuid_data0,
	 .pba_metadata_vacant_cnt = BROADCAST_SOURCE_PBA_METADATA_VACANT,
	 .pba_buf = pba_data[0]},
	{.uuid_buf = &uuid_data1,
	 .pba_metadata_vacant_cnt = BROADCAST_SOURCE_PBA_METADATA_VACANT,
	 .pba_buf = pba_data[1]}};

/**
 * @brief	Broadcast source static periodic advertising data.
 */
static struct broadcast_source_per_adv_data per_adv_data[] = {{.base_buf = &base_data0},
							      {.base_buf = &base_data1}};

static struct broadcast_source_big broadcast_param[CONFIG_BT_ISO_MAX_BIG];
static struct subgroup_config subgroups[CONFIG_BT_ISO_MAX_BIG]
				       [CONFIG_BT_BAP_BROADCAST_SRC_SUBGROUP_COUNT];

static enum bt_audio_location stream_location[CONFIG_BT_ISO_MAX_BIG]
					     [CONFIG_BT_BAP_BROADCAST_SRC_SUBGROUP_COUNT]
					     [CONFIG_BT_BAP_BROADCAST_SRC_STREAM_COUNT] = {
						     {{BT_AUDIO_LOCATION_MONO_AUDIO}}};

static uint8_t lc3_streamer_idx[CONFIG_BT_ISO_MAX_BIG][CONFIG_BT_BAP_BROADCAST_SRC_SUBGROUP_COUNT]
			       [CONFIG_BT_BAP_BROADCAST_SRC_STREAM_COUNT];

#define LC3_STREAMER_INDEX_UNUSED 0xFF

static struct bt_bap_lc3_preset *preset_find(const char *name)
{
	for (size_t i = 0; i < ARRAY_SIZE(bap_presets); i++) {
		if (strcmp(bap_presets[i].name, name) == 0) {
			return bap_presets[i].preset;
		}
	}

	return NULL;
}

static char *preset_name_find(struct bt_bap_lc3_preset *preset)
{
	for (size_t i = 0; i < ARRAY_SIZE(bap_presets); i++) {
		if (bap_presets[i].preset == preset) {
			return bap_presets[i].name;
		}
	}

	return "Custom";
}

static bool is_number(const char *str)
{
	/* Check if the string is empty */
	if (*str == '\0') {
		return 0;
	}

	uint8_t len = strlen(str);

	/* Iterate through each character of the string */
	for (uint8_t i = 0; i < len; i++) {
		/* Check if the character is not a digit */
		if (!isdigit((int)*str)) {
			return false;
		}
		str++;
	}

	/* All characters are digits */
	return true;
}

static void audio_send(void const *const data, size_t size, uint8_t num_ch)
{
	int ret;
	static int prev_ret;

	struct le_audio_encoded_audio enc_audio = {.data = data, .size = size, .num_ch = num_ch};

	ret = broadcast_source_send(0, 0, enc_audio);

	if (ret != 0 && ret != prev_ret) {
		if (ret == -ECANCELED) {
			LOG_WRN("Sending operation cancelled");
		} else {
			LOG_WRN("Problem with sending LE audio data, ret: %d", ret);
		}
	}

	prev_ret = ret;
}

static void stream_get_frame_and_send(struct stream_index stream_idx)
{
	int ret;

	uint8_t stream_file_idx =
		lc3_streamer_idx[stream_idx.lvl1][stream_idx.lvl2][stream_idx.lvl3];

	if (stream_file_idx == LC3_STREAMER_INDEX_UNUSED) {
		LOG_ERR("Stream index is unused");
		return;
	}

	const uint8_t *frame_buffer;
	int sdu_size = broadcast_param[stream_idx.lvl1]
			       .subgroups[stream_idx.lvl2]
			       .group_lc3_preset.qos.sdu;

	ret = lc3_streamer_next_frame_get(stream_file_idx, &frame_buffer);
	if (ret == -ENODATA) {
		LOG_DBG("No more frames to read");
		ret = lc3_streamer_stream_close(stream_file_idx);
		if (ret) {
			LOG_ERR("Failed to close stream: %d", ret);
		}

		lc3_streamer_idx[stream_idx.lvl1][stream_idx.lvl2][stream_idx.lvl3] =
			LC3_STREAMER_INDEX_UNUSED;

		return;
	} else if (ret == -ENOMSG) {
		LOG_ERR("Use zero packet");
		static uint8_t zero_packet[120];
		audio_send(zero_packet, 120, 1);
		return;
	} else if (ret) {
		LOG_ERR("Failed to get next frame: %d", ret);
		return;
	}

	audio_send(frame_buffer, sdu_size, 1);
}

/**
 * @brief	Handle Bluetooth LE audio events.
 */
static void le_audio_msg_sub_thread(void)
{
	int ret;
	const struct zbus_channel *chan;

	LOG_DBG("Sub thread started");

	while (1) {
		struct le_audio_msg msg;

		ret = zbus_sub_wait_msg(&le_audio_evt_sub, &chan, &msg, K_FOREVER);
		ERR_CHK(ret);

		switch (msg.event) {
		case LE_AUDIO_EVT_STREAM_SENT:
			LOG_DBG("LE_AUDIO_EVT_STREAM_SENT for stream %d.%d.%d", msg.idx.lvl1,
				msg.idx.lvl2, msg.idx.lvl3);

			stream_get_frame_and_send(msg.idx);

			break;

		case LE_AUDIO_EVT_STREAMING:
			LOG_ERR("LE audio evt streaming");

			break;

		case LE_AUDIO_EVT_NOT_STREAMING:
			LOG_ERR("LE audio evt not_streaming");

			break;

		default:
			LOG_ERR("Unexpected/unhandled le_audio event: %d", msg.event);

			break;
		}

		STACK_USAGE_PRINT("le_audio_msg_thread", &le_audio_msg_sub_thread_data);
	}
}

/**
 * @brief	Create zbus subscriber threads.
 *
 * @return	0 for success, error otherwise.
 */
static int zbus_subscribers_create(void)
{
	int ret;

	le_audio_msg_sub_thread_id = k_thread_create(
		&le_audio_msg_sub_thread_data, le_audio_msg_sub_thread_stack,
		CONFIG_LE_AUDIO_MSG_SUB_STACK_SIZE, (k_thread_entry_t)le_audio_msg_sub_thread, NULL,
		NULL, NULL, K_PRIO_PREEMPT(CONFIG_LE_AUDIO_MSG_SUB_THREAD_PRIO), 0, K_NO_WAIT);
	ret = k_thread_name_set(le_audio_msg_sub_thread_id, "LE_AUDIO_MSG_SUB");
	if (ret) {
		LOG_ERR("Failed to create le_audio_msg thread");
		return ret;
	}

	return 0;
}

/**
 * @brief	Zbus listener to receive events from bt_mgmt.
 *
 * @param[in]	chan	Zbus channel.
 *
 * @note	Will in most cases be called from BT_RX context,
 *		so there should not be too much processing done here.
 */
static void bt_mgmt_evt_handler(const struct zbus_channel *chan)
{
	int ret;
	const struct bt_mgmt_msg *msg;

	msg = zbus_chan_const_msg(chan);

	switch (msg->event) {
	case BT_MGMT_EXT_ADV_WITH_PA_READY:
		ext_adv = msg->ext_adv;

		ret = broadcast_source_start(msg->index, ext_adv);
		if (ret) {
			LOG_ERR("Failed to start broadcaster: %d", ret);
		}

		break;

	default:
		LOG_WRN("Unexpected/unhandled bt_mgmt event: %d", msg->event);
		break;
	}
}

ZBUS_LISTENER_DEFINE(bt_mgmt_evt_listen, bt_mgmt_evt_handler);

/**
 * @brief	Link zbus producers and observers.
 *
 * @return	0 for success, error otherwise.
 */
static int zbus_link_producers_observers(void)
{
	int ret;

	if (!IS_ENABLED(CONFIG_ZBUS)) {
		return -ENOTSUP;
	}

	ret = zbus_chan_add_obs(&bt_mgmt_chan, &bt_mgmt_evt_listen, ZBUS_ADD_OBS_TIMEOUT_MS);
	if (ret) {
		LOG_ERR("Failed to add bt_mgmt listener");
		return ret;
	}

	ret = zbus_chan_add_obs(&le_audio_chan, &le_audio_evt_sub, ZBUS_ADD_OBS_TIMEOUT_MS);
	if (ret) {
		LOG_ERR("Failed to add le_audio sub");
		return ret;
	}

	return 0;
}

/*
 * @brief  The following configures the data for the extended advertising.
 *         This includes the Broadcast Audio Announcements [BAP 3.7.2.1] and Broadcast_ID
 *         [BAP 3.7.2.1.1] in the AUX_ADV_IND Extended Announcements.
 *
 * @param  big_index         Index of the Broadcast Isochronous Group (BIG) to get
 *                           advertising data for.
 * @param  ext_adv_data      Pointer to the extended advertising buffers.
 * @param  ext_adv_buf       Pointer to the bt_data used for extended advertising.
 * @param  ext_adv_buf_size  Size of @p ext_adv_buf.
 * @param  ext_adv_count     Pointer to the number of elements added to @p adv_buf.
 *
 * @return  0 for success, error otherwise.
 */
static int ext_adv_populate(uint8_t big_index, struct broadcast_source_ext_adv_data *ext_adv_data,
			    struct bt_data *ext_adv_buf, size_t ext_adv_buf_size,
			    size_t *ext_adv_count)
{
	int ret;
	size_t ext_adv_buf_cnt = 0;

	ext_adv_buf[ext_adv_buf_cnt].type = BT_DATA_UUID16_SOME;
	ext_adv_buf[ext_adv_buf_cnt].data = ext_adv_data->uuid_buf->data;
	ext_adv_buf_cnt++;

	ret = bt_mgmt_manufacturer_uuid_populate(ext_adv_data->uuid_buf,
						 CONFIG_BT_DEVICE_MANUFACTURER_ID);
	if (ret) {
		LOG_ERR("Failed to add adv data with manufacturer ID: %d", ret);
		return ret;
	}

	ret = broadcast_source_ext_adv_populate(big_index, ext_adv_data,
						&ext_adv_buf[ext_adv_buf_cnt],
						ext_adv_buf_size - ext_adv_buf_cnt);
	if (ret < 0) {
		LOG_ERR("Failed to add ext adv data for broadcast source: %d", ret);
		return ret;
	}

	ext_adv_buf_cnt += ret;

	/* Add the number of UUIDs */
	ext_adv_buf[0].data_len = ext_adv_data->uuid_buf->len;

	LOG_DBG("Size of adv data: %d, num_elements: %d", sizeof(struct bt_data) * ext_adv_buf_cnt,
		ext_adv_buf_cnt);

	*ext_adv_count = ext_adv_buf_cnt;

	return 0;
}

/*
 * @brief  The following configures the data for the periodic advertising.
 *         This includes the Basic Audio Announcement, including the
 *         BASE [BAP 3.7.2.2] and BIGInfo.
 *
 * @param  big_index         Index of the Broadcast Isochronous Group (BIG) to get
 *                           advertising data for.
 * @param  pre_adv_data      Pointer to the periodic advertising buffers.
 * @param  per_adv_buf       Pointer to the bt_data used for periodic advertising.
 * @param  per_adv_buf_size  Size of @p ext_adv_buf.
 * @param  per_adv_count     Pointer to the number of elements added to @p adv_buf.
 *
 * @return  0 for success, error otherwise.
 */
static int per_adv_populate(uint8_t big_index, struct broadcast_source_per_adv_data *pre_adv_data,
			    struct bt_data *per_adv_buf, size_t per_adv_buf_size,
			    size_t *per_adv_count)
{
	int ret;
	size_t per_adv_buf_cnt = 0;

	ret = broadcast_source_per_adv_populate(big_index, pre_adv_data, per_adv_buf,
						per_adv_buf_size - per_adv_buf_cnt);
	if (ret < 0) {
		LOG_ERR("Failed to add per adv data for broadcast source: %d", ret);
		return ret;
	}

	per_adv_buf_cnt += ret;

	LOG_DBG("Size of per adv data: %d, num_elements: %d",
		sizeof(struct bt_data) * per_adv_buf_cnt, per_adv_buf_cnt);

	*per_adv_count = per_adv_buf_cnt;

	return 0;
}

static void broadcast_create(uint8_t big_index)
{
	if (big_index >= CONFIG_BT_ISO_MAX_BIG) {
		LOG_ERR("BIG index out of range");
		return;
	}

	struct broadcast_source_big *brdcst_param = &broadcast_param[big_index];

	for (size_t i = 0; i < ARRAY_SIZE(subgroups[big_index]); i++) {
		if (subgroups[big_index][i].num_bises == 0) {
			/* Default to 2 BISes */
			subgroups[big_index][i].num_bises = 2;
		}

		if (subgroups[big_index][i].context == 0) {
			subgroups[big_index][i].context = BT_AUDIO_CONTEXT_TYPE_MEDIA;
		}

		subgroups[big_index][i].location = stream_location[big_index][i];
	}

	brdcst_param->subgroups = subgroups[big_index];

	if (brdcst_param->broadcast_name[0] == '\0') {
		/* Name not set, using default */
		if (sizeof(CONFIG_BT_AUDIO_BROADCAST_NAME) > sizeof(brdcst_param->broadcast_name)) {
			LOG_ERR("Broadcast name too long");
			return;
		}

		size_t brdcst_name_size = sizeof(CONFIG_BT_AUDIO_BROADCAST_NAME) - 1;

		memcpy(brdcst_param->broadcast_name, CONFIG_BT_AUDIO_BROADCAST_NAME,
		       brdcst_name_size);
	}

	/* If no subgroups are defined, set to 1 */
	if (brdcst_param->num_subgroups == 0) {
		brdcst_param->num_subgroups = 1;
	}
}

/**
 * @brief	Clear all broadcast configurations.
 */
static void broadcast_config_clear(void)
{
	int ret;

	ret = lc3_streamer_close_all_streams();
	if (ret) {
		LOG_ERR("Failed to close all LC3 streams: %d", ret);
	}

	for (size_t i = 0; i < ARRAY_SIZE(broadcast_param); i++) {
		memset(&broadcast_param[i], 0, sizeof(broadcast_param[i]));
		for (size_t j = 0; j < ARRAY_SIZE(subgroups[i]); j++) {
			memset(&subgroups[i][j], 0, sizeof(subgroups[i][j]));
			for (size_t k = 0; k < ARRAY_SIZE(stream_location[i][j]); k++) {
				stream_location[i][j][k] = BT_AUDIO_LOCATION_MONO_AUDIO;
				lc3_streamer_idx[i][j][k] = LC3_STREAMER_INDEX_UNUSED;
			}
		}
	}
}

static void remove_cr_lf(char *str)
{
	char *p = str;

	while (*p != '\0') {
		if (*p == '\r' || *p == '\n') {
			*p = '\0';
			break;
		}
		p++;
	}
}

/* Memory intensive but simple implementation */

#define SD_FILECOUNT_MAX 80
#define SD_PATHLEN_MAX	 100
#define FOLDER_BUF_MAX	 200
#define SD_LEVEL_MAX	 3
static char sd_paths_and_files[SD_FILECOUNT_MAX][SD_PATHLEN_MAX] = {'\0'};
uint32_t num_files_added;

static int traverse_down(char *path, uint8_t level)
{
	int ret;
	char tmp_file_buf[FOLDER_BUF_MAX] = {'\0'};
	char *tmp_file_ptr = tmp_file_buf;
	size_t buf_size = FOLDER_BUF_MAX;

	if (path != NULL) {
		LOG_WRN("entered level: %d Path is %s", level, path);
	} else {
		LOG_WRN("entered level: %d Path is NULL", level);
	}

	if (level > SD_LEVEL_MAX) {
		LOG_WRN("At level %d, max level reached", level);
		return 0;
	}

	ret = sd_card_list_files(path, tmp_file_buf, &buf_size, false);
	if (ret == -ENOENT) {
		/* Not able to open, hence likely not a folder */
		return 0;
	} else if (ret) {
		return ret;
	}

	LOG_WRN("level %d tmp_file_buf is: %s", level, tmp_file_buf);

	char *token = strtok_r(tmp_file_ptr, "\r\n", &tmp_file_ptr);

	while (token != NULL) {
		remove_cr_lf(token);
		LOG_WRN("level %d, token is: %s.", level, token);
		char fullPath[SD_PATHLEN_MAX] = {'\0'};

		if (path != NULL) {
			LOG_WRN("Adding %s to %s", path, fullPath);
			remove_cr_lf(path);
			strcat(fullPath, path);
			strcat(fullPath, "/");
		} else {
			LOG_ERR("Path is NULL");
		}
		LOG_WRN("Adding %s to %s", token, fullPath);
		strcat(fullPath, token);
		LOG_WRN("Fullpath: %s", fullPath);

		if (strstr(token, ".lc3") != NULL) {
			strcpy(sd_paths_and_files[num_files_added], fullPath);
			num_files_added++;
			LOG_ERR("Added file num %d %s", num_files_added, fullPath);
			if (num_files_added >= SD_FILECOUNT_MAX) {
				LOG_WRN("Max file count reached");
			}
		} else {
			ret = traverse_down(fullPath, level + 1);
			if (ret) {
				LOG_INF("going up. Traverse down failed");
			}
		}
		token = strtok_r(NULL, "\n", &tmp_file_ptr);
	}

	LOG_INF("going up end of func from %d to %d", level, level - 1);
	return 0;
}

static int sd_card_toc_gen(void)
{
	/* Traverse SD tree */
	int ret;

	ret = traverse_down(NULL, 0);
	if (ret) {
		return ret;
	}

	LOG_WRN("back from traverse down");

	return 0;
}

int main(void)
{
	int ret;

	LOG_DBG("Main started");

	ret = nrfx_clock_divider_set(NRF_CLOCK_DOMAIN_HFCLK, NRF_CLOCK_HFCLK_DIV_1);
	ret -= NRFX_ERROR_BASE_NUM;
	if (ret) {
		return ret;
	}

	ret = bt_mgmt_init();
	ERR_CHK(ret);

	ret = zbus_link_producers_observers();
	ERR_CHK_MSG(ret, "Failed to link zbus producers and observers");

	ret = zbus_subscribers_create();
	ERR_CHK_MSG(ret, "Failed to create zbus subscriber threads");

	ret = lc3_streamer_init();
	if (ret) {
		LOG_ERR("Failed to initialize LC3 streamer: %d", ret);
		return ret;
	}

	/*TODO: Should be within the lc3_streamer_module */
	ret = sd_card_toc_gen();
	ERR_CHK_MSG(ret, "Failed to generate SD card table");

	memset(lc3_streamer_idx, LC3_STREAMER_INDEX_UNUSED, sizeof(lc3_streamer_idx));

	return 0;
}

static void context_print(const struct shell *shell)
{
	for (size_t i = 0; i < ARRAY_SIZE(contexts); i++) {
		shell_print(shell, "%s", contexts[i].name);
	}
}

static void codec_qos_print(const struct shell *shell, struct bt_audio_codec_qos *qos)
{
	if (qos->phy == BT_AUDIO_CODEC_QOS_1M || qos->phy == BT_AUDIO_CODEC_QOS_2M) {
		shell_print(shell, "\t\t\tPHY: %dM", qos->phy);
	} else if (qos->phy == BT_AUDIO_CODEC_QOS_CODED) {
		shell_print(shell, "\t\t\tPHY: LE Coded");
	} else {
		shell_print(shell, "\t\t\tPHY: Unknown");
	}

	shell_print(shell, "\t\t\tFraming: %s",
		    (qos->framing == BT_AUDIO_CODEC_QOS_FRAMING_UNFRAMED ? "unframed" : "framed"));
	shell_print(shell, "\t\t\tRTN: %d", qos->rtn);
	shell_print(shell, "\t\t\tSDU size: %d", qos->sdu);
	shell_print(shell, "\t\t\tMax Transport Latency: %d ms", qos->latency);
	shell_print(shell, "\t\t\tFrame Interval: %d us", qos->interval);
	shell_print(shell, "\t\t\tPresentation Delay: %d us", qos->pd);
}

static void broadcast_config_print(const struct shell *shell,
				   struct broadcast_source_big *brdcst_param)
{
	int ret;

	shell_print(shell, "\tAdvertising name: %s",
		    strlen(brdcst_param->adv_name) > 0 ? brdcst_param->adv_name
						       : CONFIG_BT_DEVICE_NAME);

	shell_print(shell, "\tBroadcast name: %s", brdcst_param->broadcast_name);

	shell_print(shell, "\tPacking: %s",
		    (brdcst_param->packing == BT_ISO_PACKING_INTERLEAVED ? "interleaved"
									 : "sequential"));
	shell_print(shell, "\tEncryption: %s",
		    (brdcst_param->encryption == true ? "true" : "false"));

	shell_print(shell, "\tBroadcast code: %s", brdcst_param->broadcast_code);

	for (size_t i = 0; i < brdcst_param->num_subgroups; i++) {
		struct bt_audio_codec_cfg *codec_cfg =
			&brdcst_param->subgroups[i].group_lc3_preset.codec_cfg;

		shell_print(shell, "\tSubgroup %d:", i);

		shell_print(shell, "\t\tPreset: %s", brdcst_param->subgroups[i].preset_name);

		codec_qos_print(shell, &brdcst_param->subgroups[i].group_lc3_preset.qos);

		int freq_hz = 0;

		ret = le_audio_freq_hz_get(codec_cfg, &freq_hz);
		if (ret) {
			shell_error(shell, "Failed to get sampling rate: %d", ret);
		} else {
			shell_print(shell, "\t\tSampling rate: %d Hz", freq_hz);
		}

		uint32_t bitrate = 0;

		ret = le_audio_bitrate_get(codec_cfg, &bitrate);
		if (ret) {
			shell_error(shell, "Failed to get bitrate: %d", ret);
		} else {
			shell_print(shell, "\t\tBitrate: %d bps", bitrate);
		}

		int frame_duration = 0;

		ret = le_audio_duration_us_get(codec_cfg, &frame_duration);
		if (ret) {
			shell_error(shell, "Failed to get frame duration: %d", ret);
		} else {
			shell_print(shell, "\t\tFrame duration: %d us", frame_duration);
		}

		uint32_t octets_per_sdu = 0;

		ret = le_audio_octets_per_frame_get(codec_cfg, &octets_per_sdu);
		if (ret) {
			shell_error(shell, "Failed to get octets per frame: %d", ret);
		} else {
			shell_print(shell, "\t\tOctets per frame: %d", octets_per_sdu);
		}

		int language = bt_audio_codec_cfg_meta_get_stream_lang(codec_cfg);

		if (language > 0) {
			char lang[LANGUAGE_LEN + 1] = {'\0'};

			memcpy(lang, &language, LANGUAGE_LEN);

			shell_print(shell, "\t\tLanguage: %s", lang);
		} else {
			shell_print(shell, "\t\tLanguage: not_set");
		}

		shell_print(shell, "\t\tContext(s):");

		/* Context container is a bit field with length 16 */
		for (size_t j = 0U; j < 16; j++) {
			const uint16_t bit_val = BIT(j);

			if (brdcst_param->subgroups[i].context & bit_val) {
				shell_print(shell, "\t\t\t%s", context_bit_to_str(bit_val));
			}
		}

		const unsigned char *program_info;

		ret = bt_audio_codec_cfg_meta_get_program_info(codec_cfg, &program_info);
		if (ret > 0) {
			shell_print(shell, "\t\tProgram info: %s", program_info);
		}

		int immediate =
			bt_audio_codec_cfg_meta_get_bcast_audio_immediate_rend_flag(codec_cfg);

		shell_print(shell, "\t\tImmediate rendering flag: %s",
			    (immediate == 0 ? "set" : "not set"));
		shell_print(shell, "\t\tNumber of BIS: %d", brdcst_param->subgroups[i].num_bises);
		shell_print(shell, "\t\tLocation:");

		for (size_t j = 0; j < brdcst_param->subgroups[i].num_bises; j++) {
			shell_print(shell, "\t\t\tBIS %d: %s", j,
				    location_bit_to_str(brdcst_param->subgroups[i].location[j]));
		}
	}
}

static int cmd_list(const struct shell *shell, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	for (size_t i = 0; i < ARRAY_SIZE(bap_presets); i++) {
		shell_print(shell, "%s", bap_presets[i].name);
	}

	return 0;
}

static int adv_create_and_start(const struct shell *shell, uint8_t big_index)
{
	int ret;

	size_t ext_adv_buf_cnt = 0;
	size_t per_adv_buf_cnt = 0;

	if (big_index >= CONFIG_BT_ISO_MAX_BIG) {
		shell_error(shell, "BIG index out of range");
		return -EINVAL;
	}

	if (broadcast_param[big_index].broadcast_name[0] == '\0') {
		/* Name not set, using default */
		size_t brdcst_name_size = sizeof(CONFIG_BT_AUDIO_BROADCAST_NAME) - 1;

		if (brdcst_name_size >= ARRAY_SIZE(ext_adv_data[big_index].brdcst_name_buf)) {
			shell_error(shell, "Broadcast name too long, using parts of the name");
			brdcst_name_size = ARRAY_SIZE(ext_adv_data[big_index].brdcst_name_buf) - 1;
		}

		memcpy(ext_adv_data[big_index].brdcst_name_buf, CONFIG_BT_AUDIO_BROADCAST_NAME,
		       brdcst_name_size);
	} else {
		size_t brdcst_name_size = strlen(broadcast_param[big_index].broadcast_name);

		if (brdcst_name_size >= ARRAY_SIZE(ext_adv_data[big_index].brdcst_name_buf)) {
			shell_error(shell, "Broadcast name too long, using parts of the name");
			brdcst_name_size = ARRAY_SIZE(ext_adv_data[big_index].brdcst_name_buf) - 1;
		}

		memcpy(ext_adv_data[big_index].brdcst_name_buf,
		       broadcast_param[big_index].broadcast_name, brdcst_name_size);
	}

	if (broadcast_param[big_index].adv_name[0] == '\0') {
		/* Name not set, using default */
		size_t adv_name_size = sizeof(CONFIG_BT_AUDIO_BROADCAST_NAME) - 1;

		memcpy(broadcast_param[big_index].adv_name, CONFIG_BT_AUDIO_BROADCAST_NAME,
		       adv_name_size);
	}

	bt_set_name(broadcast_param[big_index].adv_name);

	/* Get advertising set for BIG0 */
	ret = ext_adv_populate(big_index, &ext_adv_data[big_index], ext_adv_buf[big_index],
			       ARRAY_SIZE(ext_adv_buf[big_index]), &ext_adv_buf_cnt);
	if (ret) {
		return ret;
	}

	ret = per_adv_populate(big_index, &per_adv_data[big_index], &per_adv_buf[big_index], 1,
			       &per_adv_buf_cnt);
	if (ret) {
		return ret;
	}

	/* Start broadcaster */
	ret = bt_mgmt_adv_start(big_index, ext_adv_buf[big_index], ext_adv_buf_cnt,
				&per_adv_buf[big_index], per_adv_buf_cnt, false);
	if (ret) {
		shell_error(shell, "Failed to start advertising for BIG%d", big_index);
		return ret;
	}

	return 0;
}

/**
 * @brief	Converts the arguments to BIG and subgroup indexes.
 *
 * @param[in]	shell		Pointer to the shell instance.
 * @param[in]	argc		Number of arguments.
 * @param[in]	argv		Pointer to the arguments.
 * @param[out]	big_index	Pointer to the BIG index.
 * @param[in]	argv_big	Index of the BIG index in the arguments.
 * @param[out]	subgroup_index	Pointer to the subgroup index.
 * @param[in]	argv_subgroup	Index of the subgroup index in the arguments.
 *
 * @return	0 for success, error otherwise.
 */
static int argv_to_indexes(const struct shell *shell, size_t argc, char **argv, uint8_t *big_index,
			   uint8_t argv_big, uint8_t *subgroup_index, uint8_t argv_subgroup)
{

	if (big_index == NULL) {
		shell_error(shell, "BIG index not provided");
		return -EINVAL;
	}

	if (argv_big >= argc) {
		shell_error(shell, "BIG index not provided");
		return -EINVAL;
	}

	if (!is_number(argv[argv_big])) {
		shell_error(shell, "BIG index must be a digit");
		return -EINVAL;
	}

	*big_index = (uint8_t)atoi(argv[argv_big]);

	if (*big_index >= CONFIG_BT_ISO_MAX_BIG) {
		shell_error(shell, "BIG index out of range");
		return -EINVAL;
	}

	if (subgroup_index != NULL) {
		if (argv_subgroup >= argc) {
			shell_error(shell, "Subgroup index not provided");
			return -EINVAL;
		}

		if (!is_number(argv[argv_subgroup])) {
			shell_error(shell, "Subgroup index must be a digit");
			return -EINVAL;
		}

		*subgroup_index = (uint8_t)atoi(argv[argv_subgroup]);

		if (*subgroup_index >= CONFIG_BT_BAP_BROADCAST_SRC_SUBGROUP_COUNT) {
			shell_error(shell, "Subgroup index exceeds max value: %d",
				    CONFIG_BT_BAP_BROADCAST_SRC_SUBGROUP_COUNT - 1);
			return -EINVAL;
		}

		if (broadcast_param[*big_index].subgroups == NULL) {
			shell_error(shell, "No subgroups defined for BIG%d", *big_index);
			return -EINVAL;
		}

		if (*subgroup_index >= broadcast_param[*big_index].num_subgroups) {
			shell_error(shell, "Subgroup index out of range");
			return -EINVAL;
		}
	}

	return 0;
}

static int enable_big(const struct shell *shell, uint8_t big_index)
{
	int ret;

	if (big_index >= CONFIG_BT_ISO_MAX_BIG) {
		return -EINVAL;
	}

	if ((broadcast_param[big_index].subgroups == NULL) ||
	    (broadcast_param[big_index].num_subgroups == 0)) {
		LOG_ERR("No subgroups defined for BIG%d", big_index);
		return -EINVAL;
	}

	ret = broadcast_source_enable(&broadcast_param[big_index], big_index);
	if (ret) {
		shell_error(shell, "Failed to enable broadcaster: %d", ret);
		return ret;
	}

	ret = adv_create_and_start(shell, big_index);
	if (ret) {
		shell_error(shell, "Failed to start advertising for BIG%d: %d", big_index, ret);
		return ret;
	}

	/* TODO: Workaround this. Must find a new way. */
	k_msleep(100);

	for (int i = 0; i < broadcast_param[big_index].num_subgroups; i++) {
		for (int j = 0; j < broadcast_param[big_index].subgroups[i].num_bises; j++) {
			LOG_ERR("Starting stream for BIG%d, subgroup %d, BIS %d", big_index, i, j);
			stream_get_frame_and_send((struct stream_index){big_index, i, j});
		}
	}

	return 0;
}

static int cmd_start(const struct shell *shell, size_t argc, char **argv)
{
	int ret;

	if (argc == 2) {
		uint8_t big_index;

		ret = argv_to_indexes(shell, argc, argv, &big_index, 1, NULL, 0);
		if (ret) {
			return ret;
		}

		ret = enable_big(shell, big_index);
		if (ret) {
			return ret;
		}

	} else {
		for (int i = 0; i < CONFIG_BT_ISO_MAX_BIG; i++) {
			if (broadcast_param[i].subgroups != NULL ||
			    broadcast_param[i].num_subgroups > 0) {
				ret = enable_big(shell, i);
				if (ret) {
					return ret;
				}
			}
		}
	}

	return 0;
}

static int broadcaster_stop(const struct shell *shell, uint8_t big_index)
{
	int ret;

	if (!broadcast_source_is_streaming(big_index)) {
		return 0;
	}

	ret = broadcast_source_disable(big_index);
	if (ret) {
		shell_error(shell, "Failed to stop broadcaster(s) %d", ret);
		return ret;
	}

	ret = bt_mgmt_per_adv_stop(big_index);
	if (ret) {
		shell_error(shell, "Failed to stop periodic advertiser");
		return ret;
	}

	ret = bt_mgmt_ext_adv_stop(big_index);
	if (ret) {
		shell_error(shell, "Failed to stop extended advertiser");
		return ret;
	}

	if (big_index == 0) {
		net_buf_simple_reset(&base_data0);
		net_buf_simple_reset(&uuid_data0);
	} else if (big_index == 1) {
		net_buf_simple_reset(&base_data1);
		net_buf_simple_reset(&uuid_data1);
	} else {
		shell_error(shell, "BIG index out of range");
		return -EINVAL;
	}

	per_adv_buf[big_index].data_len = 0;
	per_adv_buf[big_index].type = 0;

	for (size_t j = 0; j < ARRAY_SIZE(ext_adv_buf[big_index]); j++) {
		ext_adv_buf[big_index][j].data_len = 0;
		ext_adv_buf[big_index][j].type = 0;
	}

	return 0;
}

static int cmd_stop(const struct shell *shell, size_t argc, char **argv)
{
	int ret;

	if (argc == 2) {
		uint8_t big_index;

		ret = argv_to_indexes(shell, argc, argv, &big_index, 1, NULL, 0);
		if (ret) {
			return ret;
		}

		ret = broadcaster_stop(shell, big_index);

	} else {
		for (size_t i = 0; i < CONFIG_BT_ISO_MAX_BIG; i++) {
			if (broadcast_param[i].subgroups != NULL) {
				ret = broadcaster_stop(shell, i);
				if (ret) {
					return ret;
				}
			}
		}
	}

	return 0;
}

static int cmd_show(const struct shell *shell, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	for (size_t i = 0; i < CONFIG_BT_ISO_MAX_BIG; i++) {
		if (broadcast_param[i].subgroups == NULL) {
			continue;
		}

		bool streaming = broadcast_source_is_streaming(i);

		shell_print(shell, "BIG %d:", i);
		shell_print(shell, "\tStreaming: %s", (streaming ? "true" : "false"));

		broadcast_config_print(shell, &broadcast_param[i]);

		shell_print(shell, "\t\tFiles:");

		for (size_t j = 0; j < broadcast_param[i].num_subgroups; j++) {
			for (size_t k = 0; k < broadcast_param[i].subgroups[j].num_bises; k++) {

				uint8_t streamer_idx = lc3_streamer_idx[i][j][k];

				if (streamer_idx == LC3_STREAMER_INDEX_UNUSED) {
					shell_print(shell, "\t\t\tBIS %d: Not set", k);
				} else {
					char file_name[30];
					bool looping = lc3_streamer_is_looping(streamer_idx);

					lc3_streamer_file_path_get(streamer_idx, file_name,
								   sizeof(file_name));
					shell_print(shell, "\t\t\tBIS %d: %s %s", j, file_name,
						    looping ? "(looping)" : "");
				}
			}
		}
	}

	return 0;
}

static int cmd_preset(const struct shell *shell, size_t argc, char **argv)
{
	int ret;
	uint8_t big_index;

	if ((argc >= 2) && (strcmp(argv[1], "print") == 0)) {
		for (size_t i = 0; i < ARRAY_SIZE(bap_presets); i++) {
			shell_print(shell, "%s", bap_presets[i].name);
		}
		return 0;
	}

	if (argc < 3) {
		shell_error(shell,
			    "Usage: bct preset <preset> <BIG index> optional:<subgroup index>");
		return -EINVAL;
	}

	struct bt_bap_lc3_preset *preset = preset_find(argv[1]);

	if (!preset) {
		shell_error(shell,
			    "Preset not found, use 'bct preset print' to see available presets");
		return -EINVAL;
	}

	ret = argv_to_indexes(shell, argc, argv, &big_index, 2, NULL, 0);
	if (ret) {
		return ret;
	}

	broadcast_create(big_index);

	if (argc == 4) {
		uint8_t sub_index;

		ret = argv_to_indexes(shell, argc, argv, &big_index, 2, &sub_index, 3);

		broadcast_param[big_index].subgroups[sub_index].group_lc3_preset = *preset;
		broadcast_param[big_index].subgroups[sub_index].preset_name =
			preset_name_find(preset);
	} else {
		for (size_t i = 0; i < broadcast_param[big_index].num_subgroups; i++) {
			broadcast_param[big_index].subgroups[i].group_lc3_preset = *preset;
			broadcast_param[big_index].subgroups[i].preset_name =
				preset_name_find(preset);
		}
	}

	return 0;
}

static int cmd_packing(const struct shell *shell, size_t argc, char **argv)
{
	int ret;
	uint8_t big_index;

	if (argc < 3) {
		shell_error(shell, "Usage: bct packing <seq/int> <BIG index>");
		return -EINVAL;
	}

	ret = argv_to_indexes(shell, argc, argv, &big_index, 2, NULL, 0);
	if (ret) {
		return ret;
	}

	if (strcmp(argv[1], "seq") == 0) {
		broadcast_param[big_index].packing = BT_ISO_PACKING_SEQUENTIAL;
	} else if (strcmp(argv[1], "int") == 0) {
		broadcast_param[big_index].packing = BT_ISO_PACKING_INTERLEAVED;
	} else {
		shell_error(shell, "Invalid packing type");
		return -EINVAL;
	}

	return 0;
}

static int cmd_lang_set(const struct shell *shell, size_t argc, char **argv)
{
	int ret;
	uint8_t big_index;
	uint8_t sub_index;

	if (argc < 4) {
		shell_error(shell, "Usage: bct lang_set <language> <BIG index> <subgroup index>");
		return -EINVAL;
	}

	if (strlen(argv[1]) != LANGUAGE_LEN) {
		shell_error(shell, "Language must be %d characters long", LANGUAGE_LEN);
		return -EINVAL;
	}

	char *language = argv[1];

	ret = argv_to_indexes(shell, argc, argv, &big_index, 2, &sub_index, 3);
	if (ret) {
		return ret;
	}

	bt_audio_codec_cfg_meta_set_stream_lang(
		&broadcast_param[big_index].subgroups[sub_index].group_lc3_preset.codec_cfg,
		sys_get_le24(language));

	return 0;
}

static int cmd_immediate_set(const struct shell *shell, size_t argc, char **argv)
{
	int ret;
	uint8_t big_index;
	uint8_t sub_index;

	if (argc < 3) {
		shell_error(shell, "Usage: bct immediate <0/1> <BIG index> <subgroup index>");
		return -EINVAL;
	}

	bool immediate = strtoul(argv[1], NULL, 10);

	if ((immediate != 0) && (immediate != 1)) {
		shell_error(shell, "Immediate must be 0 or 1");
		return -EINVAL;
	}

	ret = argv_to_indexes(shell, argc, argv, &big_index, 2, &sub_index, 3);
	if (ret) {
		return ret;
	}

	if (immediate) {
		ret = bt_audio_codec_cfg_meta_set_bcast_audio_immediate_rend_flag(
			&broadcast_param[big_index]
				 .subgroups[sub_index]
				 .group_lc3_preset.codec_cfg);
		if (ret < 0) {
			shell_error(shell, "Failed to set immediate rendering flag: %d", ret);
			return ret;
		}
	} else {
		ret = bt_audio_codec_cfg_meta_unset_val(
			&broadcast_param[big_index].subgroups[sub_index].group_lc3_preset.codec_cfg,
			BT_AUDIO_METADATA_TYPE_BROADCAST_IMMEDIATE);
		if (ret < 0) {
			shell_error(shell, "Failed to unset immediate rendering flag: %d", ret);
			return ret;
		}
	}

	return 0;
}

static int cmd_num_subgroups(const struct shell *shell, size_t argc, char **argv)
{
	int ret;
	uint8_t big_index;

	if (argc < 3) {
		shell_error(shell, "Usage: bct num_subgroups <num> <BIG index>");
		return -EINVAL;
	}

	if (!is_number(argv[1])) {
		shell_error(shell, "Number of subgroups must be a digit");
		return -EINVAL;
	}

	uint8_t num_subgroups = (uint8_t)atoi(argv[1]);

	if (num_subgroups > CONFIG_BT_BAP_BROADCAST_SRC_SUBGROUP_COUNT) {
		shell_error(shell, "Max allowed subgroups is: %d",
			    CONFIG_BT_BAP_BROADCAST_SRC_SUBGROUP_COUNT);
		return -EINVAL;
	}

	ret = argv_to_indexes(shell, argc, argv, &big_index, 2, NULL, 0);
	if (ret) {
		return ret;
	}

	broadcast_param[big_index].num_subgroups = num_subgroups;

	return 0;
}

static int cmd_num_bises(const struct shell *shell, size_t argc, char **argv)
{
	int ret;
	uint8_t big_index;
	uint8_t sub_index;

	if (argc < 4) {
		shell_error(shell, "Usage: bct num_bises <num> <BIG index> <subgroup index>");
		return -EINVAL;
	}

	if (!is_number(argv[1])) {
		shell_error(shell, "Number of BISes must be a digit");
		return -EINVAL;
	}

	uint8_t num_bises = (uint8_t)atoi(argv[1]);

	if (!is_number(argv[2])) {
		shell_error(shell, "BIG index must be a digit");
		return -EINVAL;
	}

	if (num_bises > CONFIG_BT_BAP_BROADCAST_SRC_STREAM_COUNT) {
		shell_error(shell, "Max allowed BISes is: %d",
			    CONFIG_BT_BAP_BROADCAST_SRC_STREAM_COUNT);
		return -EINVAL;
	}

	ret = argv_to_indexes(shell, argc, argv, &big_index, 2, &sub_index, 3);
	if (ret) {
		return ret;
	}

	broadcast_param[big_index].subgroups[sub_index].num_bises = num_bises;

	return 0;
}

static int cmd_context(const struct shell *shell, size_t argc, char **argv)
{
	int ret;
	uint8_t big_index;
	uint8_t sub_index;
	uint16_t context = 0;

	if ((argc >= 2) && (strcmp(argv[1], "print") == 0)) {
		context_print(shell);
		return 0;
	}

	if (argc < 4) {
		shell_error(shell, "Usage: bct context <context> <BIG index> <subgroup index>");
		return -EINVAL;
	}

	for (size_t i = 0; i < ARRAY_SIZE(contexts); i++) {
		if (strcasecmp(argv[1], contexts[i].name) == 0) {
			context = contexts[i].context;
			break;
		}
	}

	if (!context) {
		shell_error(shell, "Context not found, use bct context print to see "
				   "available contexts");
		return -EINVAL;
	}

	ret = argv_to_indexes(shell, argc, argv, &big_index, 2, &sub_index, 3);
	if (ret) {
		return ret;
	}

	broadcast_param[big_index].subgroups[sub_index].context = context;

	return 0;
}

static int location_find(const struct shell *shell, const char *name)
{
	for (size_t i = 0; i < ARRAY_SIZE(locations); i++) {
		if (strcasecmp(locations[i].name, name) == 0) {
			return locations[i].location;
		}
	}

	shell_error(shell, "Location not found");

	return -ESRCH;
}

static void location_print(const struct shell *shell)
{
	for (size_t i = 0; i < ARRAY_SIZE(locations); i++) {
		shell_print(shell, "%s - %s", locations[i].name,
			    location_bit_to_str(locations[i].location));
	}
}

static int cmd_location(const struct shell *shell, size_t argc, char **argv)
{
	int ret;
	uint8_t big_index;
	uint8_t sub_index;

	if ((argc >= 2) && (strcmp(argv[1], "print") == 0)) {
		location_print(shell);
		return 0;
	}

	if (argc < 4) {
		shell_error(shell, "Usage: bct location <location> <BIG index> <subgroup "
				   "index> <BIS index>");
		return -EINVAL;
	}

	ret = argv_to_indexes(shell, argc, argv, &big_index, 2, &sub_index, 3);
	if (ret) {
		return ret;
	}

	if (!is_number(argv[4])) {
		shell_error(shell, "BIS index must be a digit");
		return -EINVAL;
	}

	uint8_t bis_index = (uint8_t)atoi(argv[4]);

	if ((bis_index >= CONFIG_BT_BAP_BROADCAST_SRC_STREAM_COUNT) ||
	    (bis_index >= broadcast_param[big_index].subgroups[sub_index].num_bises)) {
		shell_error(shell, "BIS index out of range");
		return -EINVAL;
	}

	int location = location_find(shell, argv[1]);

	if (location < 0) {
		return -EINVAL;
	}

	broadcast_param[big_index].subgroups[sub_index].location[bis_index] = location;

	return 0;
}

static int cmd_broadcast_name(const struct shell *shell, size_t argc, char **argv)
{
	int ret;
	uint8_t big_index;

	if (argc < 2) {
		shell_error(shell, "Usage: bct name <name> <BIG index>");
		return -EINVAL;
	}

	ret = argv_to_indexes(shell, argc, argv, &big_index, 2, NULL, 0);
	if (ret) {
		return ret;
	}

	size_t name_length = strlen(argv[1]);

	/* Leave space for null termination */
	if (name_length >= ARRAY_SIZE(broadcast_param[big_index].broadcast_name) - 1) {
		shell_error(shell, "Name too long");
		return -EINVAL;
	}

	/* Delete old name if set */
	memset(broadcast_param[big_index].broadcast_name, '\0',
	       ARRAY_SIZE(broadcast_param[big_index].broadcast_name));
	memcpy(broadcast_param[big_index].broadcast_name, argv[1], name_length);

	return 0;
}

static int cmd_encrypt(const struct shell *shell, size_t argc, char **argv)
{
	int ret;
	uint8_t big_index;

	if (argc < 3) {
		shell_error(shell,
			    "Usage: bct encrypt <0/1> <BIG index> Optional:<broadcast_code>");
		return -EINVAL;
	}

	if (!is_number(argv[1])) {
		shell_error(shell, "Encryption must be 0 or 1");
		return -EINVAL;
	}

	uint8_t encrypt = (uint8_t)atoi(argv[1]);

	if (encrypt != 1 && encrypt != 0) {
		shell_error(shell, "Encryption must be 0 or 1");
		return -EINVAL;
	}

	ret = argv_to_indexes(shell, argc, argv, &big_index, 2, NULL, 0);
	if (ret) {
		return ret;
	}

	broadcast_param[big_index].encryption = encrypt;

	if (encrypt == 1) {
		if (argc < 4) {
			shell_error(shell, "Broadcast code must be set");
			return -EINVAL;
		}

		if (strlen(argv[3]) > BT_AUDIO_BROADCAST_CODE_SIZE) {
			shell_error(shell, "Broadcast code must be %d characters long",
				    BT_AUDIO_BROADCAST_CODE_SIZE);
			return -EINVAL;
		}
		memset(broadcast_param[big_index].broadcast_code, '\0',
		       ARRAY_SIZE(broadcast_param[big_index].broadcast_code));
		memcpy(broadcast_param[big_index].broadcast_code, argv[3], strlen(argv[3]));
	}

	return 0;
}

static int cmd_adv_name(const struct shell *shell, size_t argc, char **argv)
{
	int ret;
	uint8_t big_index;

	if (argc < 2) {
		shell_error(shell, "Usage: bct device_name <name> <BIG index>");
		return -EINVAL;
	}

	if (strlen(argv[1]) > CONFIG_BT_DEVICE_NAME_MAX) {
		shell_error(shell, "Device name too long");
		return -EINVAL;
	}

	ret = argv_to_indexes(shell, argc, argv, &big_index, 2, NULL, 0);
	if (ret) {
		return ret;
	}

	memset(broadcast_param[big_index].adv_name, '\0',
	       ARRAY_SIZE(broadcast_param[big_index].adv_name));
	memcpy(broadcast_param[big_index].adv_name, argv[1], strlen(argv[1]));

	return 0;
}

static int cmd_program_info(const struct shell *shell, size_t argc, char **argv)
{
	int ret;
	uint8_t big_index;
	uint8_t sub_index;

	if (argc != 4) {
		shell_error(shell, "Usage: bct program_info \"info\" <BIG index> <subgroup index>");
		return -EINVAL;
	}

	ret = argv_to_indexes(shell, argc, argv, &big_index, 2, &sub_index, 3);
	if (ret) {
		return ret;
	}

	ret = bt_audio_codec_cfg_meta_set_program_info(
		&broadcast_param[big_index].subgroups[sub_index].group_lc3_preset.codec_cfg,
		argv[1], strlen(argv[1]));
	if (ret < 0) {
		shell_error(shell, "Failed to set program info: %d", ret);
		return ret;
	}

	return 0;
}

#define FILE_LIST_BUF_SIZE 512
static int cmd_file_list(const struct shell *shell, size_t argc, char **argv)
{
	int ret;
	char buf[FILE_LIST_BUF_SIZE];
	size_t buf_size = FILE_LIST_BUF_SIZE;
	char *dir_path = NULL;

	if (argc > 2) {
		shell_error(shell, "Usage: bct file list [dir path]");
		return -EINVAL;
	}

	if (argc == 2) {
		dir_path = argv[1];
	}

	ret = sd_card_list_files(dir_path, buf, &buf_size, true);
	if (ret) {
		shell_error(shell, "List files err: %d", ret);
		return ret;
	}

	shell_print(shell, "%s", buf);

	return 0;
}

static int cmd_file_select(const struct shell *shell, size_t argc, char **argv)
{
	int ret;
	uint8_t big_idx;
	uint8_t sub_idx;
	uint8_t bis_idx;

	if (argc < 4) {
		shell_error(shell,
			    "Usage: bct file select <file path> <BIG index> <subgroup index> "
			    "<BIS index> [loop file 1/0]");
		return -EINVAL;
	}

	ret = argv_to_indexes(shell, argc, argv, &big_idx, 2, &sub_idx, 3);

	if (!is_number(argv[4])) {
		shell_error(shell, "BIS index must be a digit");
		return -EINVAL;
	}

	bis_idx = (uint8_t)atoi(argv[4]);

	if (bis_idx >= CONFIG_BT_BAP_BROADCAST_SRC_STREAM_COUNT) {
		shell_error(shell, "BIS index out of range");
		return -EINVAL;
	}

	char *file_name = argv[1];
	bool loop_file = false;

	if (argc == 6) {
		bool valid_argument =
			is_number(argv[5]) && ((atoi(argv[5]) == 1) || (atoi(argv[5]) == 0));

		if (!valid_argument) {
			shell_error(shell, "Loop file must be 0 or 1");
			return -EINVAL;
		}

		loop_file = (atoi(argv[5]) == 1);
	}

	LOG_INF("Selecting file %s for stream %d.%d.%d", file_name, big_idx, sub_idx, bis_idx);

	ret = lc3_streamer_stream_register(file_name, &lc3_streamer_idx[big_idx][sub_idx][bis_idx],
					   loop_file);
	if (ret) {
		shell_error(shell, "Failed to register stream: %d", ret);
		return ret;
	}

	if (broadcast_source_is_streaming(big_idx)) {
		stream_get_frame_and_send((struct stream_index){big_idx, sub_idx, bis_idx});
	}

	return 0;
}

static int cmd_phy(const struct shell *shell, size_t argc, char **argv)
{
	int ret;
	uint8_t big_index;
	uint8_t sub_index;

	if (argc != 4) {
		shell_error(shell,
			    "Usage: bct phy <1, 2 or 4 (coded)> <BIG index> <subgroup index>");
		return -EINVAL;
	}

	if (!is_number(argv[1])) {
		shell_error(shell, "PHY must be a digit");
		return -EINVAL;
	}

	uint8_t phy = (uint8_t)atoi(argv[1]);

	if (phy != BT_AUDIO_CODEC_QOS_1M && phy != BT_AUDIO_CODEC_QOS_2M &&
	    phy != BT_AUDIO_CODEC_QOS_CODED) {
		shell_error(shell, "Invalid PHY");
		return -EINVAL;
	}

	ret = argv_to_indexes(shell, argc, argv, &big_index, 2, &sub_index, 3);
	if (ret) {
		return ret;
	}

	broadcast_param[big_index].subgroups[sub_index].group_lc3_preset.qos.phy = phy;
	broadcast_param[big_index].subgroups[sub_index].preset_name = "Custom";

	return 0;
}

static int cmd_framing(const struct shell *shell, size_t argc, char **argv)
{
	int ret;
	uint8_t big_index;
	uint8_t sub_index;

	if (argc != 4) {
		shell_error(shell,
			    "Usage: bct framing <unframed/framed> <BIG index> <subgroup index>");
		return -EINVAL;
	}

	ret = argv_to_indexes(shell, argc, argv, &big_index, 2, &sub_index, 3);
	if (ret) {
		return ret;
	}

	if (strcasecmp(argv[1], "unframed") == 0) {
		broadcast_param[big_index].subgroups[sub_index].group_lc3_preset.qos.framing =
			BT_AUDIO_CODEC_QOS_FRAMING_UNFRAMED;
		broadcast_param[big_index].subgroups[sub_index].preset_name = "Custom";
	} else if (strcasecmp(argv[1], "framed") == 0) {
		broadcast_param[big_index].subgroups[sub_index].group_lc3_preset.qos.framing =
			BT_AUDIO_CODEC_QOS_FRAMING_FRAMED;
		broadcast_param[big_index].subgroups[sub_index].preset_name = "Custom";
	} else {
		shell_error(shell, "Invalid framing type");
		return -EINVAL;
	}

	return 0;
}

static int cmd_rtn(const struct shell *shell, size_t argc, char **argv)
{
	int ret;
	uint8_t big_index;
	uint8_t sub_index;

	if (argc != 4) {
		shell_error(shell, "Usage: bct rtn <num> <BIG index> <subgroup index>");
		return -EINVAL;
	}

	if (!is_number(argv[1])) {
		shell_error(shell, "RTN must be a digit");
		return -EINVAL;
	}

	if (strtoul(argv[1], NULL, 10) > UINT8_MAX) {
		shell_error(shell, "RTN must be less than %d", UINT8_MAX);
		return -EINVAL;
	}

	uint8_t rtn = (uint8_t)atoi(argv[1]);

	ret = argv_to_indexes(shell, argc, argv, &big_index, 2, &sub_index, 3);
	if (ret) {
		return ret;
	}

	broadcast_param[big_index].subgroups[sub_index].group_lc3_preset.qos.rtn = rtn;
	broadcast_param[big_index].subgroups[sub_index].preset_name = "Custom";

	return 0;
}

static int cmd_sdu(const struct shell *shell, size_t argc, char **argv)
{
	int ret;
	uint8_t big_index;
	uint8_t sub_index;

	if (argc != 4) {
		shell_error(shell, "Usage: bct sdu <octets> <BIG index> <subgroup index>");
		return -EINVAL;
	}

	if (!is_number(argv[1])) {
		shell_error(shell, "SDU must be a digit");
		return -EINVAL;
	}

	if (strtoul(argv[1], NULL, 10) > UINT16_MAX) {
		shell_error(shell, "SDU must be less than %d", UINT16_MAX);
		return -EINVAL;
	}

	uint16_t sdu = (uint16_t)atoi(argv[1]);

	ret = argv_to_indexes(shell, argc, argv, &big_index, 2, &sub_index, 3);
	if (ret) {
		return ret;
	}

	broadcast_param[big_index].subgroups[sub_index].group_lc3_preset.qos.sdu = sdu;
	broadcast_param[big_index].subgroups[sub_index].preset_name = "Custom";

	return 0;
}

static int cmd_mtl(const struct shell *shell, size_t argc, char **argv)
{
	int ret;
	uint8_t big_index;
	uint8_t sub_index;

	if (argc != 4) {
		shell_error(shell, "Usage: bct mtl <time in ms> <BIG index> <subgroup index>");
		return -EINVAL;
	}

	if (!is_number(argv[1])) {
		shell_error(shell, "MTL must be a digit");
		return -EINVAL;
	}

	if (strtoul(argv[1], NULL, 10) > UINT16_MAX) {
		shell_error(shell, "MTL must be less than %d", UINT16_MAX);
		return -EINVAL;
	}

	uint16_t mtl = (uint16_t)atoi(argv[1]);

	ret = argv_to_indexes(shell, argc, argv, &big_index, 2, &sub_index, 3);
	if (ret) {
		return ret;
	}

	broadcast_param[big_index].subgroups[sub_index].group_lc3_preset.qos.latency = mtl;
	broadcast_param[big_index].subgroups[sub_index].preset_name = "Custom";

	return 0;
}

static int cmd_frame_interval(const struct shell *shell, size_t argc, char **argv)
{
	int ret;
	uint8_t big_index;
	uint8_t sub_index;

	if (argc != 4) {
		shell_error(shell,
			    "Usage: bct frame_interval <time in us> <BIG index> <subgroup index>");
		return -EINVAL;
	}

	if (!is_number(argv[1])) {
		shell_error(shell, "Frame interval must be a digit");
		return -EINVAL;
	}

	if (strtoul(argv[1], NULL, 10) > UINT32_MAX) {
		shell_error(shell, "Frame interval must be less than %ld", UINT32_MAX);
		return -EINVAL;
	}

	uint32_t frame_interval = (uint32_t)atoi(argv[1]);

	if (frame_interval != 7500 && frame_interval != 10000) {
		shell_error(shell, "Frame interval must be 7500 or 10000");
		return -EINVAL;
	}

	ret = argv_to_indexes(shell, argc, argv, &big_index, 2, &sub_index, 3);
	if (ret) {
		return ret;
	}

	broadcast_param[big_index].subgroups[sub_index].group_lc3_preset.qos.interval =
		frame_interval;
	broadcast_param[big_index].subgroups[sub_index].preset_name = "Custom";

	return 0;
}

static int cmd_pd(const struct shell *shell, size_t argc, char **argv)
{
	int ret;
	uint8_t big_index;
	uint8_t sub_index;

	if (argc != 4) {
		shell_error(shell, "Usage: bct pd <time us> <BIG index> <subgroup index>");
		return -EINVAL;
	}

	if (!is_number(argv[1])) {
		shell_error(shell, "presentation delay must be a digit");
		return -EINVAL;
	}

	if (strtoul(argv[1], NULL, 10) > UINT32_MAX) {
		shell_error(shell, "Presentation delay must be less than %ld", UINT32_MAX);
		return -EINVAL;
	}

	uint32_t pd = (uint32_t)atoi(argv[1]);

	ret = argv_to_indexes(shell, argc, argv, &big_index, 2, &sub_index, 3);
	if (ret) {
		return ret;
	}

	broadcast_param[big_index].subgroups[sub_index].group_lc3_preset.qos.pd = pd;
	broadcast_param[big_index].subgroups[sub_index].preset_name = "Custom";

	return 0;
}

static int usecase_find(const struct shell *shell, const char *name)
{
	for (size_t i = 0; i < ARRAY_SIZE(pre_defined_use_cases); i++) {
		if (strcasecmp(pre_defined_use_cases[i].name, name) == 0) {
			return pre_defined_use_cases[i].use_case;
		}
	}

	shell_error(shell, "Use case not found");

	return -ESRCH;
}

static void usecase_print(const struct shell *shell)
{
	for (size_t i = 0; i < ARRAY_SIZE(pre_defined_use_cases); i++) {
		shell_print(shell, "%d - %s", pre_defined_use_cases[i].use_case,
			    pre_defined_use_cases[i].name);
	}
}

static void lecture_set(const struct shell *shell)
{
	char *preset_argv[3] = {"preset", "24_2_1", "0"};
	char *adv_name_argv[3] = {"adv_name", "Lecture hall", "0"};
	char *name_argv[3] = {"name", "Lecture", "0"};
	char *packing_argv[3] = {"packing", "int", "0"};

	char *lang_argv[4] = {"lang_set", "eng", "0", "0"};

	char *context_argv[4] = {"context", "live", "0", "0"};

	char *program_info_argv[4] = {"program_info", "Mathematics 101", "0", "0"};

	char *imm_argv[4] = {"immediate", "1", "0", "0"};

	char *num_bis_argv[4] = {"num_bises", "1", "0", "0"};

	cmd_preset(shell, 3, preset_argv);
	cmd_adv_name(shell, 3, adv_name_argv);
	cmd_broadcast_name(shell, 3, name_argv);
	cmd_packing(shell, 3, packing_argv);

	cmd_lang_set(shell, 4, lang_argv);

	cmd_context(shell, 4, context_argv);

	cmd_program_info(shell, 4, program_info_argv);

	cmd_immediate_set(shell, 4, imm_argv);

	cmd_num_bises(shell, 4, num_bis_argv);

	cmd_show(shell, 0, NULL);
}

static void silent_tv_1_set(const struct shell *shell)
{
	char *preset_argv[3] = {"preset", "24_2_1", "0"};
	char *adv_name_argv[3] = {"adv_name", "Silent TV1", "0"};
	char *name_argv[3] = {"name", "TV-audio", "0"};
	char *packing_argv[3] = {"packing", "int", "0"};
	char *encrypt_argv[4] = {"encrypt", "1", "0", "Auratest"};

	char *context_argv[4] = {"context", "media", "0", "0"};

	char *program_info_argv[4] = {"program_info", "News", "0", "0"};

	char *imm_argv[4] = {"immediate", "1", "0", "0"};

	char *num_bis_argv[4] = {"num_bises", "2", "0", "0"};

	char *location_FL_argv[5] = {"location", "FL", "0", "0", "0"};
	char *location_FR_argv[5] = {"location", "FR", "0", "0", "1"};

	cmd_preset(shell, 3, preset_argv);
	cmd_adv_name(shell, 3, adv_name_argv);
	cmd_broadcast_name(shell, 3, name_argv);
	cmd_packing(shell, 3, packing_argv);
	cmd_encrypt(shell, 4, encrypt_argv);

	cmd_context(shell, 4, context_argv);

	cmd_program_info(shell, 4, program_info_argv);

	cmd_immediate_set(shell, 4, imm_argv);

	cmd_num_bises(shell, 4, num_bis_argv);

	cmd_location(shell, 5, location_FL_argv);
	cmd_location(shell, 5, location_FR_argv);

	cmd_show(shell, 0, NULL);
}

static void silent_tv_2_set(const struct shell *shell)
{
	/* BIG0 */
	char *preset0_argv[3] = {"preset", "24_2_1", "0"};
	char *adv_name0_argv[3] = {"adv_name", "Silent_TV2_std", "0"};
	char *name0_argv[3] = {"name", "TV - standard qual.", "0"};
	char *packing0_argv[3] = {"packing", "int", "0"};
	char *encrypt0_argv[4] = {"encrypt", "1", "0", "Auratest"};

	char *context0_argv[4] = {"context", "media", "0", "0"};

	char *program_info0_argv[4] = {"program_info", "Biathlon", "0", "0"};

	char *imm0_argv[4] = {"immediate", "1", "0", "0"};

	char *num_bis0_argv[4] = {"num_bises", "2", "0", "0"};

	char *location_FL0_argv[5] = {"location", "FL", "0", "0", "0"};
	char *location_FR0_argv[5] = {"location", "FR", "0", "0", "1"};

	cmd_preset(shell, 3, preset0_argv);
	cmd_adv_name(shell, 3, adv_name0_argv);
	cmd_broadcast_name(shell, 3, name0_argv);
	cmd_packing(shell, 3, packing0_argv);
	cmd_encrypt(shell, 4, encrypt0_argv);

	cmd_context(shell, 4, context0_argv);

	cmd_program_info(shell, 4, program_info0_argv);

	cmd_immediate_set(shell, 4, imm0_argv);

	cmd_num_bises(shell, 4, num_bis0_argv);

	cmd_location(shell, 5, location_FL0_argv);
	cmd_location(shell, 5, location_FR0_argv);

	/* BIG1 */
	char *preset1_argv[3] = {"preset", "48_2_1", "1"};
	char *adv_name1_argv[3] = {"adv_name", "Silent_TV2_high", "1"};
	char *name1_argv[3] = {"name", "TV - high qual.", "1"};
	char *packing1_argv[3] = {"packing", "int", "1"};
	char *encrypt1_argv[4] = {"encrypt", "1", "1", "Auratest"};

	char *context1_argv[4] = {"context", "media", "1", "0"};

	char *program_info1_argv[4] = {"program_info", "Biathlon", "1", "0"};

	char *imm1_argv[4] = {"immediate", "1", "1", "0"};

	char *num_bis1_argv[4] = {"num_bises", "2", "1", "0"};

	char *location_FL1_argv[5] = {"location", "FL", "1", "0", "0"};
	char *location_FR1_argv[5] = {"location", "FR", "1", "0", "1"};

	cmd_preset(shell, 3, preset1_argv);
	cmd_adv_name(shell, 3, adv_name1_argv);
	cmd_broadcast_name(shell, 3, name1_argv);
	cmd_packing(shell, 3, packing1_argv);
	cmd_encrypt(shell, 4, encrypt1_argv);

	cmd_context(shell, 4, context1_argv);

	cmd_program_info(shell, 4, program_info1_argv);

	cmd_immediate_set(shell, 4, imm1_argv);

	cmd_num_bises(shell, 4, num_bis1_argv);

	cmd_location(shell, 5, location_FL1_argv);
	cmd_location(shell, 5, location_FR1_argv);

	cmd_show(shell, 0, NULL);
}

static void multi_language_set(const struct shell *shell)
{
	char *num_subs_argv[3] = {"num_subgroups", "3", "0"};
	char *preset_argv[3] = {"preset", "24_2_2", "0"};
	char *adv_name_argv[3] = {"adv_name", "Multi-language", "0"};
	char *name_argv[3] = {"name", "Multi-language", "0"};
	char *packing_argv[3] = {"packing", "int", "0"};

	char *lang0_argv[4] = {"lang_set", "eng", "0", "0"};
	char *lang1_argv[4] = {"lang_set", "spa", "0", "1"};
	char *lang2_argv[4] = {"lang_set", "nor", "0", "2"};

	char *context0_argv[4] = {"context", "unspecified", "0", "0"};
	char *context1_argv[4] = {"context", "unspecified", "0", "1"};
	char *context2_argv[4] = {"context", "unspecified", "0", "2"};

	char *num_bis0_argv[4] = {"num_bises", "1", "0", "0"};
	char *num_bis1_argv[4] = {"num_bises", "1", "0", "1"};
	char *num_bis2_argv[4] = {"num_bises", "1", "0", "2"};

	cmd_num_subgroups(shell, 3, num_subs_argv);
	cmd_preset(shell, 3, preset_argv);
	cmd_adv_name(shell, 3, adv_name_argv);
	cmd_broadcast_name(shell, 3, name_argv);
	cmd_packing(shell, 3, packing_argv);

	cmd_lang_set(shell, 4, lang0_argv);
	cmd_lang_set(shell, 4, lang1_argv);
	cmd_lang_set(shell, 4, lang2_argv);

	cmd_context(shell, 4, context0_argv);
	cmd_context(shell, 4, context1_argv);
	cmd_context(shell, 4, context2_argv);

	cmd_num_bises(shell, 4, num_bis0_argv);
	cmd_num_bises(shell, 4, num_bis1_argv);
	cmd_num_bises(shell, 4, num_bis2_argv);

	cmd_show(shell, 0, NULL);
}

static void personal_sharing_set(const struct shell *shell)
{
	char *preset_argv[3] = {"preset", "48_2_2", "0"};
	char *adv_name_argv[3] = {"adv_name", "John's phone", "0"};
	char *name_argv[3] = {"name", "Personal sharing", "0"};
	char *packing_argv[3] = {"packing", "int", "0"};
	char *encrypt_argv[4] = {"encrypt", "1", "0", "Auratest"};

	char *context_argv[4] = {"context", "conversational", "0", "0"};

	char *num_bis_argv[4] = {"num_bises", "2", "0", "0"};

	char *location_FL_argv[5] = {"location", "FL", "0", "0", "0"};
	char *location_FR_argv[5] = {"location", "FR", "0", "0", "1"};

	cmd_preset(shell, 3, preset_argv);
	cmd_adv_name(shell, 3, adv_name_argv);
	cmd_broadcast_name(shell, 3, name_argv);
	cmd_packing(shell, 3, packing_argv);
	cmd_encrypt(shell, 4, encrypt_argv);

	cmd_context(shell, 4, context_argv);

	cmd_num_bises(shell, 4, num_bis_argv);

	cmd_location(shell, 5, location_FL_argv);
	cmd_location(shell, 5, location_FR_argv);

	cmd_show(shell, 0, NULL);
}

static void personal_multi_language_set(const struct shell *shell)
{
	char *num_subs_argv[3] = {"num_subgroups", "2", "0"};
	char *preset_argv[3] = {"preset", "24_2_2", "0"};
	char *adv_name_argv[3] = {"adv_name", "Brian's laptop", "0"};
	char *name_argv[3] = {"name", "Personal multi-lang.", "0"};
	char *packing_argv[3] = {"packing", "int", "0"};
	char *encrypt_argv[4] = {"encrypt", "1", "0", "Auratest"};

	char *lang0_argv[4] = {"lang_set", "eng", "0", "0"};
	char *lang1_argv[4] = {"lang_set", "spa", "0", "1"};

	char *context0_argv[4] = {"context", "media", "0", "0"};
	char *context1_argv[4] = {"context", "media", "0", "1"};

	char *num_bis0_argv[4] = {"num_bises", "2", "0", "0"};
	char *num_bis1_argv[4] = {"num_bises", "2", "0", "1"};

	char *location_FL0_argv[5] = {"location", "FL", "0", "0", "0"};
	char *location_FR0_argv[5] = {"location", "FR", "0", "0", "1"};
	char *location_FL1_argv[5] = {"location", "FL", "0", "1", "0"};
	char *location_FR1_argv[5] = {"location", "FR", "0", "1", "1"};

	cmd_num_subgroups(shell, 3, num_subs_argv);
	cmd_preset(shell, 3, preset_argv);
	cmd_adv_name(shell, 3, adv_name_argv);
	cmd_broadcast_name(shell, 3, name_argv);
	cmd_packing(shell, 3, packing_argv);
	cmd_encrypt(shell, 4, encrypt_argv);

	cmd_lang_set(shell, 4, lang0_argv);
	cmd_lang_set(shell, 4, lang1_argv);

	cmd_context(shell, 4, context0_argv);
	cmd_context(shell, 4, context1_argv);

	cmd_num_bises(shell, 4, num_bis0_argv);
	cmd_num_bises(shell, 4, num_bis1_argv);

	cmd_location(shell, 5, location_FL0_argv);
	cmd_location(shell, 5, location_FR0_argv);
	cmd_location(shell, 5, location_FL1_argv);
	cmd_location(shell, 5, location_FR1_argv);

	cmd_show(shell, 0, NULL);
}

static int cmd_usecase(const struct shell *shell, size_t argc, char **argv)
{
	if ((argc >= 2) && (strcmp(argv[1], "print") == 0)) {
		usecase_print(shell);
		return 0;
	}

	if (argc < 2) {
		shell_error(shell, "Usage: bct usecase <use_case> - Either index or text");
		return -EINVAL;
	}

	int use_case_nr = -1;

	if (is_number(argv[1])) {
		use_case_nr = atoi(argv[1]);
	} else {
		use_case_nr = usecase_find(shell, argv[1]);

		if (use_case_nr < 0) {
			return -EINVAL;
		}
	}

	/* Clear previous configurations */
	broadcast_config_clear();

	switch (use_case_nr) {
	case LECTURE:
		lecture_set(shell);
		break;
	case SILENT_TV_1:
		silent_tv_1_set(shell);
		break;
	case SILENT_TV_2:
		silent_tv_2_set(shell);
		break;
	case MULTI_LANGUAGE:
		multi_language_set(shell);
		break;
	case PERSONAL_SHARING:
		personal_sharing_set(shell);
		break;
	case PERSONAL_MULTI_LANGUAGE:
		personal_multi_language_set(shell);
		break;
	default:
		shell_error(shell, "Use case not found");
		break;
	}

	return 0;
}

static int cmd_clear(const struct shell *shell, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	broadcast_config_clear();

	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_file_cmd,
			       SHELL_COND_CMD(CONFIG_SHELL, list, NULL, "List files on SD card",
					      cmd_file_list),
			       SHELL_COND_CMD(CONFIG_SHELL, select, NULL, "Select file on SD card",
					      cmd_file_select),
			       SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(
	configuration_cmd, SHELL_COND_CMD(CONFIG_SHELL, list, NULL, "List presets", cmd_list),
	SHELL_COND_CMD(CONFIG_SHELL, start, NULL, "Start broadcaster", cmd_start),
	SHELL_COND_CMD(CONFIG_SHELL, stop, NULL, "Stop broadcaster", cmd_stop),
	SHELL_COND_CMD(CONFIG_SHELL, show, NULL, "Show current configuration", cmd_show),
	SHELL_COND_CMD(CONFIG_SHELL, packing, NULL, "Set type of packing", cmd_packing),
	SHELL_COND_CMD(CONFIG_SHELL, preset, NULL, "Set preset", cmd_preset),
	SHELL_COND_CMD(CONFIG_SHELL, lang, NULL, "Set language", cmd_lang_set),
	SHELL_COND_CMD(CONFIG_SHELL, immediate, NULL, "Set immediate rendering flag",
		       cmd_immediate_set),
	SHELL_COND_CMD(CONFIG_SHELL, num_subgroups, NULL, "Set number of subgroups",
		       cmd_num_subgroups),
	SHELL_COND_CMD(CONFIG_SHELL, num_bises, NULL, "Set number of BISes", cmd_num_bises),
	SHELL_COND_CMD(CONFIG_SHELL, context, NULL, "Set context", cmd_context),
	SHELL_COND_CMD(CONFIG_SHELL, location, NULL, "Set location", cmd_location),
	SHELL_COND_CMD(CONFIG_SHELL, broadcast_name, NULL, "Set broadcast name",
		       cmd_broadcast_name),
	SHELL_COND_CMD(CONFIG_SHELL, encrypt, NULL, "Set broadcast code", cmd_encrypt),
	SHELL_COND_CMD(CONFIG_SHELL, usecase, NULL, "Set use case", cmd_usecase),
	SHELL_COND_CMD(CONFIG_SHELL, clear, NULL, "Clear configuration", cmd_clear),
	SHELL_COND_CMD(CONFIG_SHELL, adv_name, NULL, "Set advertising name", cmd_adv_name),
	SHELL_COND_CMD(CONFIG_SHELL, program_info, NULL, "Set program info", cmd_program_info),
	SHELL_COND_CMD(CONFIG_SHELL, phy, NULL, "Set PHY", cmd_phy),
	SHELL_COND_CMD(CONFIG_SHELL, framing, NULL, "Set framing", cmd_framing),
	SHELL_COND_CMD(CONFIG_SHELL, rtn, NULL, "Set number of re-transmits", cmd_rtn),
	SHELL_COND_CMD(CONFIG_SHELL, sdu, NULL, "Set SDU (octets)", cmd_sdu),
	SHELL_COND_CMD(CONFIG_SHELL, mtl, NULL, "Set Max Transport latency (ms)", cmd_mtl),
	SHELL_COND_CMD(CONFIG_SHELL, frame_interval, NULL, "Set frame interval (us)",
		       cmd_frame_interval),
	SHELL_COND_CMD(CONFIG_SHELL, pd, NULL, "Set presentation delay (us)", cmd_pd),
	SHELL_COND_CMD(CONFIG_SHELL, file, &sub_file_cmd, "File commands", NULL),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(bct, &configuration_cmd, "Broadcast Configuration Tool", NULL);

static int cmd_autocomplete(const struct shell *sh, size_t argc, char *argv[])
{

	LOG_WRN("cmd_erase %s %s", argv[0], argv[1]);
}

static void file_paths_get(size_t idx, struct shell_static_entry *entry);

SHELL_DYNAMIC_CMD_CREATE(folder_names, file_paths_get);

static void file_paths_get(size_t idx, struct shell_static_entry *entry)
{
	if (idx < num_files_added) {
		entry->syntax = sd_paths_and_files[idx];
	} else {
		entry->syntax = NULL;
	}
	entry->handler = NULL;
	entry->help = NULL;
	entry->subcmd = &folder_names;
}

SHELL_STATIC_SUBCMD_SET_CREATE(flash_cmds,
			       SHELL_CMD_ARG(autocomplete, &folder_names, "autocomplete",
					     cmd_autocomplete, 2, 2),
			       SHELL_SUBCMD_SET_END);

static int cmd_dummy(const struct shell *sh, size_t argc, char **argv)
{
	shell_error(sh, "%s:unknown parameter: %s", argv[0], argv[1]);
	return -EINVAL;
}

SHELL_CMD_ARG_REGISTER(listfiles, &flash_cmds, "autocomplete test", cmd_dummy, 2, 0);
