/*
 * Copyright (c) 2018 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "sw_codec_select.h"

#include <zephyr/kernel.h>
#include <errno.h>

#include "channel_assignment.h"
#include "pcm_stream_channel_modifier.h"
#if (CONFIG_SW_CODEC_LC3)
#include "sw_codec_lc3.h"
#endif /* (CONFIG_SW_CODEC_LC3) */

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(sw_codec_select, CONFIG_SW_CODEC_SELECT_LOG_LEVEL);

static struct sw_codec_config m_config;

#ifdef CONFIG_AUDIO_SAMPLE_RATE_CONVERSION
#include "sample_rate_converter.h"
static struct sample_rate_converter_ctx sample_rate_converter_left;
static struct sample_rate_converter_ctx sample_rate_converter_right;
#endif /* CONFIG_SAMPLE_RATE_CONVERSION */

bool sw_codec_is_initialized(void)
{
	return m_config.initialized;
}

int sw_codec_encode(void *pcm_data, size_t pcm_size, uint8_t **encoded_data, size_t *encoded_size)
{
	int ret;

	/* Temp storage for split stereo PCM signal */
	char pcm_data_mono_system_sample_rate[AUDIO_CH_NUM][PCM_NUM_BYTES_MONO] = {0};
	/* Make sure we have enough space for two frames (stereo) */
	static uint8_t m_encoded_data[ENC_MAX_FRAME_SIZE * AUDIO_CH_NUM];

#ifdef CONFIG_AUDIO_SAMPLE_RATE_CONVERSION
	char pcm_data_mono[AUDIO_CH_NUM][PCM_NUM_BYTES_MONO] = {0};
#endif /* CONFIG_AUDIO_SAMPLE_RATE_CONVERSION */

	size_t pcm_block_size_mono_system_sample_rate;
	size_t pcm_block_size_mono;

	if (!m_config.encoder.enabled) {
		LOG_ERR("Encoder has not been initialized");
		return -ENXIO;
	}

	switch (m_config.sw_codec) {
	case SW_CODEC_LC3: {
#if (CONFIG_SW_CODEC_LC3)
		uint16_t encoded_bytes_written;

		/* Since LC3 is a single channel codec, we must split the
		 * stereo PCM stream
		 */
		ret = pscm_two_channel_split(pcm_data, pcm_size, CONFIG_AUDIO_BIT_DEPTH_BITS,
					     pcm_data_mono_system_sample_rate[AUDIO_CH_L],
					     pcm_data_mono_system_sample_rate[AUDIO_CH_R],
					     &pcm_block_size_mono_system_sample_rate);
		if (ret) {
			return ret;
		}

		switch (m_config.encoder.channel_mode) {
		case SW_CODEC_MONO: {
			char *lc3_in_data_left_ptr;

			if (m_config.encoder.sample_rate_hz == CONFIG_AUDIO_SAMPLE_RATE_HZ) {
				lc3_in_data_left_ptr = pcm_data_mono_system_sample_rate[AUDIO_CH_L];
				pcm_block_size_mono = pcm_block_size_mono_system_sample_rate;
			} else {
#ifdef CONFIG_AUDIO_SAMPLE_RATE_CONVERSION
				ret = sample_rate_converter_process(
					&sample_rate_converter_left, SAMPLE_RATE_FILTER_TEST,
					(void *)pcm_data_mono_system_sample_rate[AUDIO_CH_L],
					pcm_block_size_mono_system_sample_rate,
					CONFIG_AUDIO_SAMPLE_RATE_HZ,
					(void *)pcm_data_mono[AUDIO_CH_L],
					ARRAY_SIZE(pcm_data_mono[AUDIO_CH_L]), &pcm_block_size_mono,
					m_config.encoder.sample_rate_hz);
				if (ret) {
					LOG_ERR("Sample rate conversion failed for left channel: "
						"%d",
						ret);
					return ret;
				}
				lc3_in_data_left_ptr = pcm_data_mono[AUDIO_CH_L];
#else
				LOG_ERR("Sample rates are not equal");
				return -EINVAL;
#endif /* CONFIG_AUDIO_SAMPLE_RATE_CONVERSION */
			}

			ret = sw_codec_lc3_enc_run(lc3_in_data_left_ptr, pcm_block_size_mono,
						   LC3_USE_BITRATE_FROM_INIT, 0,
						   sizeof(m_encoded_data), m_encoded_data,
						   &encoded_bytes_written);
			if (ret) {
				return ret;
			}
			break;
		}
		case SW_CODEC_STEREO: {
			char *lc3_in_data_left_ptr;
			char *lc3_in_data_right_ptr;

			if (m_config.encoder.sample_rate_hz == CONFIG_AUDIO_SAMPLE_RATE_HZ) {
				lc3_in_data_left_ptr = pcm_data_mono_system_sample_rate[AUDIO_CH_L];
				lc3_in_data_right_ptr =
					pcm_data_mono_system_sample_rate[AUDIO_CH_R];
				pcm_block_size_mono = pcm_block_size_mono_system_sample_rate;
			} else {
#ifdef CONFIG_AUDIO_SAMPLE_RATE_CONVERSION
				ret = sample_rate_converter_process(
					&sample_rate_converter_left, SAMPLE_RATE_FILTER_TEST,
					(void *)pcm_data_mono_system_sample_rate[AUDIO_CH_L],
					pcm_block_size_mono_system_sample_rate,
					CONFIG_AUDIO_SAMPLE_RATE_HZ,
					(void *)pcm_data_mono[AUDIO_CH_L],
					ARRAY_SIZE(pcm_data_mono[AUDIO_CH_L]), &pcm_block_size_mono,
					m_config.encoder.sample_rate_hz);
				if (ret) {
					LOG_ERR("Sample rate conversion failed for left channel: "
						"%d",
						ret);
					return ret;
				}
				lc3_in_data_left_ptr = pcm_data_mono[AUDIO_CH_L];

				ret = sample_rate_converter_process(
					&sample_rate_converter_right, SAMPLE_RATE_FILTER_TEST,
					(void *)pcm_data_mono_system_sample_rate[AUDIO_CH_R],
					pcm_block_size_mono_system_sample_rate,
					CONFIG_AUDIO_SAMPLE_RATE_HZ,
					(void *)pcm_data_mono[AUDIO_CH_R],
					ARRAY_SIZE(pcm_data_mono[AUDIO_CH_R]), &pcm_block_size_mono,
					m_config.encoder.sample_rate_hz);
				if (ret) {
					LOG_ERR("Sample rate conversion failed for right channel: "
						"%d",
						ret);
					return ret;
				}
				lc3_in_data_right_ptr = pcm_data_mono[AUDIO_CH_R];
#else
				LOG_ERR("Sample rates does not equal");
				return -EINVAL;
#endif /* CONFIG_AUDIO_SAMPLE_RATE_CONVERSION */
			}

			ret = sw_codec_lc3_enc_run(lc3_in_data_left_ptr, pcm_block_size_mono,
						   LC3_USE_BITRATE_FROM_INIT, AUDIO_CH_L,
						   sizeof(m_encoded_data), m_encoded_data,
						   &encoded_bytes_written);
			if (ret) {
				return ret;
			}

			ret = sw_codec_lc3_enc_run(lc3_in_data_right_ptr, pcm_block_size_mono,
						   LC3_USE_BITRATE_FROM_INIT, AUDIO_CH_R,
						   sizeof(m_encoded_data) - encoded_bytes_written,
						   m_encoded_data + encoded_bytes_written,
						   &encoded_bytes_written);
			if (ret) {
				return ret;
			}
			encoded_bytes_written += encoded_bytes_written;
			break;
		}
		default:
			LOG_ERR("Unsupported channel mode for encoder: %d",
				m_config.encoder.channel_mode);
			return -ENODEV;
		}

		*encoded_data = m_encoded_data;
		*encoded_size = encoded_bytes_written;

#endif /* (CONFIG_SW_CODEC_LC3) */
		break;
	}
	default:
		LOG_ERR("Unsupported codec: %d", m_config.sw_codec);
		return -ENODEV;
	}

	return 0;
}

int sw_codec_decode(uint8_t const *const encoded_data, size_t encoded_size, bool bad_frame,
		    void **decoded_data, size_t *decoded_size)
{
	if (!m_config.decoder.enabled) {
		LOG_ERR("Decoder has not been initialized");
		return -ENXIO;
	}

	int ret;

	static char pcm_data_stereo[PCM_NUM_BYTES_STEREO];

	size_t pcm_size_stereo = 0;
	size_t pcm_size_mono = 0;
	size_t decoded_data_size = 0;

	switch (m_config.sw_codec) {
	case SW_CODEC_LC3: {
#if (CONFIG_SW_CODEC_LC3)
		switch (m_config.decoder.channel_mode) {
		case SW_CODEC_MONO: {
			char *pcm_data_mono_left_ptr;

			char pcm_data_mono_left[PCM_NUM_BYTES_MONO] = {0};
			char decoded_data_mono_left[PCM_NUM_BYTES_MONO] = {0};

#ifdef CONFIG_AUDIO_SAMPLE_RATE_CONVERSION
			char decoded_data_mono_left_converted[PCM_NUM_BYTES_MONO] = {0};
#endif /* CONFIG_AUDIO_SAMPLE_RATE_CONVERSION */

			if (bad_frame && IS_ENABLED(CONFIG_SW_CODEC_OVERRIDE_PLC)) {
				memset(pcm_data_mono_left, 0, PCM_NUM_BYTES_MONO);
				pcm_size_mono = PCM_NUM_BYTES_MONO;
			} else {
				ret = sw_codec_lc3_dec_run(
					encoded_data, encoded_size, LC3_PCM_NUM_BYTES_MONO, 0,
					decoded_data_mono_left, (uint16_t *)&decoded_data_size,
					bad_frame);
				if (ret) {
					return ret;
				}

				if (m_config.decoder.sample_rate_hz ==
				    CONFIG_AUDIO_SAMPLE_RATE_HZ) {
					pcm_data_mono_left_ptr = decoded_data_mono_left;
					pcm_size_mono = decoded_data_size;
				} else {
#ifdef CONFIG_AUDIO_SAMPLE_RATE_CONVERSION
					ret = sample_rate_converter_process(
						&sample_rate_converter_left,
						SAMPLE_RATE_FILTER_TEST,
						(void *)decoded_data_mono_left, decoded_data_size,
						m_config.decoder.sample_rate_hz,
						(void *)decoded_data_mono_left_converted,
						ARRAY_SIZE(decoded_data_mono_left_converted),
						&pcm_size_mono, CONFIG_AUDIO_SAMPLE_RATE_HZ);
					if (ret) {
						LOG_ERR("Sample rate conversion "
							"failed for left "
							"channel: "
							"%d",
							ret);
						return ret;
					}
					pcm_data_mono_left_ptr = decoded_data_mono_left_converted;
#else
					LOG_ERR("Sample rates are not equal");
					return -EINVAL;
#endif /* CONFIG_AUDIO_SAMPLE_RATE_CONVERSION */
				}
			}

			/* For now, i2s is only stereo, so in order to send
			 * just one channel, we need to insert 0 for the
			 * other channel
			 */
			ret = pscm_zero_pad(pcm_data_mono_left_ptr, pcm_size_mono,
					    m_config.decoder.audio_ch, CONFIG_AUDIO_BIT_DEPTH_BITS,
					    pcm_data_stereo, &pcm_size_stereo);
			if (ret) {
				return ret;
			}
			break;
		}
		case SW_CODEC_STEREO: {
			char *pcm_data_mono_left_ptr;
			char *pcm_data_mono_right_ptr;

			char pcm_data_mono_left[PCM_NUM_BYTES_MONO] = {0};
			char pcm_data_mono_right[PCM_NUM_BYTES_MONO] = {0};

			char decoded_data_mono_left[PCM_NUM_BYTES_MONO] = {0};
			char decoded_data_mono_right[PCM_NUM_BYTES_MONO] = {0};

#ifdef CONFIG_AUDIO_SAMPLE_RATE_CONVERSION
			char decoded_data_mono_left_converted[PCM_NUM_BYTES_MONO] = {0};
			char decoded_data_mono_right_converted[PCM_NUM_BYTES_MONO] = {0};
#endif /* CONFIG_AUDIO_SAMPLE_RATE_CONVERSION */

			if (bad_frame && IS_ENABLED(CONFIG_SW_CODEC_OVERRIDE_PLC)) {
				memset(pcm_data_mono_left, 0, PCM_NUM_BYTES_MONO);
				memset(pcm_data_mono_right, 0, PCM_NUM_BYTES_MONO);
				decoded_data_size = PCM_NUM_BYTES_MONO;
			} else {
				/* Decode left channel */
				ret = sw_codec_lc3_dec_run(
					encoded_data, encoded_size / 2, LC3_PCM_NUM_BYTES_MONO,
					AUDIO_CH_L, decoded_data_mono_left,
					(uint16_t *)&decoded_data_size, bad_frame);
				if (ret) {
					return ret;
				}

				if (m_config.decoder.sample_rate_hz ==
				    CONFIG_AUDIO_SAMPLE_RATE_HZ) {
					pcm_data_mono_left_ptr = decoded_data_mono_left;
					pcm_size_mono = decoded_data_size;
				} else {
#ifdef CONFIG_AUDIO_SAMPLE_RATE_CONVERSION
					ret = sample_rate_converter_process(
						&sample_rate_converter_left,
						SAMPLE_RATE_FILTER_TEST,
						(void *)decoded_data_mono_left, decoded_data_size,
						m_config.decoder.sample_rate_hz,
						(void *)decoded_data_mono_left_converted,
						ARRAY_SIZE(decoded_data_mono_left_converted),
						&pcm_size_mono, CONFIG_AUDIO_SAMPLE_RATE_HZ);
					if (ret) {
						LOG_ERR("Sample rate conversion "
							"failed for left "
							"channel: "
							"%d",
							ret);
						return ret;
					}
					pcm_data_mono_left_ptr = decoded_data_mono_left_converted;
#else
					LOG_ERR("Sample rates are not equal");
					return -EINVAL;
#endif /* CONFIG_AUDIO_SAMPLE_RATE_CONVERSION */
				}

				/* Decode right channel */
				ret = sw_codec_lc3_dec_run(
					(encoded_data + (encoded_size / 2)), encoded_size / 2,
					LC3_PCM_NUM_BYTES_MONO, AUDIO_CH_R, decoded_data_mono_right,
					(uint16_t *)&decoded_data_size, bad_frame);
				if (ret) {
					return ret;
				}

				if (m_config.decoder.sample_rate_hz ==
				    CONFIG_AUDIO_SAMPLE_RATE_HZ) {
					pcm_data_mono_right_ptr = decoded_data_mono_right;
					pcm_size_mono = decoded_data_size;
				} else {
#ifdef CONFIG_AUDIO_SAMPLE_RATE_CONVERSION
					ret = sample_rate_converter_process(
						&sample_rate_converter_right,
						SAMPLE_RATE_FILTER_TEST,
						(void *)decoded_data_mono_right, decoded_data_size,
						m_config.decoder.sample_rate_hz,
						(void *)decoded_data_mono_right_converted,
						ARRAY_SIZE(decoded_data_mono_right_converted),
						&pcm_size_mono, CONFIG_AUDIO_SAMPLE_RATE_HZ);
					if (ret) {
						LOG_ERR("Sample rate conversion "
							"failed for left "
							"channel: "
							"%d",
							ret);
						return ret;
					}
					pcm_data_mono_right_ptr = decoded_data_mono_right_converted;
#else
					LOG_ERR("Sample rates are not equal");
					return -EINVAL;
#endif /* CONFIG_AUDIO_SAMPLE_RATE_CONVERSION */
				}
			}

			ret = pscm_combine(pcm_data_mono_left_ptr, pcm_data_mono_right_ptr,
					   pcm_size_mono, CONFIG_AUDIO_BIT_DEPTH_BITS,
					   pcm_data_stereo, &pcm_size_stereo);
			if (ret) {
				return ret;
			}
			break;
		}
		default:
			LOG_ERR("Unsupported channel mode for decoder: %d",
				m_config.decoder.channel_mode);
			return -ENODEV;
		}

		*decoded_size = pcm_size_stereo;
		*decoded_data = pcm_data_stereo;
#endif /* (CONFIG_SW_CODEC_LC3) */
		break;
	}
	default:
		LOG_ERR("Unsupported codec: %d", m_config.sw_codec);
		return -ENODEV;
	}
	return 0;
}

int sw_codec_uninit(struct sw_codec_config sw_codec_cfg)
{
	int ret;

	if (m_config.sw_codec != sw_codec_cfg.sw_codec) {
		LOG_ERR("Trying to uninit a codec that is not first initialized");
		return -ENODEV;
	}
	switch (m_config.sw_codec) {
	case SW_CODEC_LC3:
#if (CONFIG_SW_CODEC_LC3)
		if (sw_codec_cfg.encoder.enabled) {
			if (!m_config.encoder.enabled) {
				LOG_ERR("Trying to uninit encoder, it has not been "
					"initialized");
				return -EALREADY;
			}
			ret = sw_codec_lc3_enc_uninit_all();
			if (ret) {
				return ret;
			}
			m_config.encoder.enabled = false;
		}

		if (sw_codec_cfg.decoder.enabled) {
			if (!m_config.decoder.enabled) {
				LOG_WRN("Trying to uninit decoder, it has not been "
					"initialized");
				return -EALREADY;
			}

			ret = sw_codec_lc3_dec_uninit_all();
			if (ret) {
				return ret;
			}
			m_config.decoder.enabled = false;
		}
#endif /* (CONFIG_SW_CODEC_LC3) */
		break;
	default:
		LOG_ERR("Unsupported codec: %d", m_config.sw_codec);
		return false;
	}

	m_config.initialized = false;

	return 0;
}

int sw_codec_init(struct sw_codec_config sw_codec_cfg)
{
	int ret;

	switch (sw_codec_cfg.sw_codec) {
	case SW_CODEC_LC3: {
#if (CONFIG_SW_CODEC_LC3)
		if (m_config.sw_codec != SW_CODEC_LC3) {
			/* Check if LC3 is already initialized */
			ret = sw_codec_lc3_init(NULL, NULL, CONFIG_AUDIO_FRAME_DURATION_US);
			if (ret) {
				return ret;
			}
		}

		if (sw_codec_cfg.encoder.enabled) {
			if (m_config.encoder.enabled) {
				LOG_WRN("The LC3 encoder is already initialized");
				return -EALREADY;
			}
			uint16_t pcm_bytes_req_enc;

			LOG_DBG("Encode: %dHz %dbits %dus %dbps %d channel(s)",
				sw_codec_cfg.encoder.sample_rate_hz, CONFIG_AUDIO_BIT_DEPTH_BITS,
				CONFIG_AUDIO_FRAME_DURATION_US, sw_codec_cfg.encoder.bitrate,
				sw_codec_cfg.encoder.num_ch);

			ret = sw_codec_lc3_enc_init(
				sw_codec_cfg.encoder.sample_rate_hz, CONFIG_AUDIO_BIT_DEPTH_BITS,
				CONFIG_AUDIO_FRAME_DURATION_US, sw_codec_cfg.encoder.bitrate,
				sw_codec_cfg.encoder.num_ch, &pcm_bytes_req_enc);

			if (ret) {
				return ret;
			}
		}

		if (sw_codec_cfg.decoder.enabled) {
			if (m_config.decoder.enabled) {
				LOG_WRN("The LC3 decoder is already initialized");
				return -EALREADY;
			}

			LOG_DBG("Decode: %dHz %dbits %dus %d channel(s)",
				sw_codec_cfg.decoder.sample_rate_hz, CONFIG_AUDIO_BIT_DEPTH_BITS,
				CONFIG_AUDIO_FRAME_DURATION_US, sw_codec_cfg.decoder.num_ch);

			ret = sw_codec_lc3_dec_init(
				sw_codec_cfg.decoder.sample_rate_hz, CONFIG_AUDIO_BIT_DEPTH_BITS,
				CONFIG_AUDIO_FRAME_DURATION_US, sw_codec_cfg.decoder.num_ch);

			if (ret) {
				return ret;
			}
		}
		break;
#else
		LOG_ERR("LC3 is not compiled in, please open menuconfig and select "
			"LC3");
		return -ENODEV;
#endif /* (CONFIG_SW_CODEC_LC3) */
	}

	default:
		LOG_ERR("Unsupported codec: %d", sw_codec_cfg.sw_codec);
		return false;
	}

#ifdef CONFIG_AUDIO_SAMPLE_RATE_CONVERSION
	ret = sample_rate_converter_open(&sample_rate_converter_left);
	if (ret) {
		LOG_ERR("Failed to initialize sample rate converter for left "
			"channel %d",
			ret);
		return ret;
	}

	ret = sample_rate_converter_open(&sample_rate_converter_right);
	if (ret) {
		LOG_ERR("Failed to initialize sample rate converter for right "
			"channel %d",
			ret);
		return ret;
	}
#endif /* CONFIG_AUDIO_SAMPLE_RATE_CONVERSION */

	m_config = sw_codec_cfg;
	m_config.initialized = true;

	return 0;
}
