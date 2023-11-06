
#ifndef __SAMPLE_RATE_CONVERTER_FILTER__
#define __SAMPLE_RATE_CONVERTER_FILTER__

#include <dsp/filtering_functions.h>

void get_filter(enum sample_rate_converter_filter filter_type, uint8_t conversion_ratio,
		q15_t **filter_ptr, size_t *filter_size);

#endif /* __SAMPLE_RATE_CONVERTER_FILTER__ */
