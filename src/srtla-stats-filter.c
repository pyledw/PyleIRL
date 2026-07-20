#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>
#include <stdio.h>
#include <stdlib.h>

// Forward declarations
extern void srtla_populate_receivers_list(obs_property_t *p);
extern uint64_t srtla_get_total_bytes(int listen_port);

struct srtla_stats_filter {
	obs_source_t *context;
	int listen_port;
	uint64_t last_bytes;
	uint64_t last_time_ms;
};

static const char *srtla_stats_filter_get_name(void *type_data)
{
	(void)type_data;
	return "SRTLA Bitrate Monitor";
}

static void *srtla_stats_filter_create(obs_data_t *settings, obs_source_t *context)
{
	struct srtla_stats_filter *filter = bzalloc(sizeof(struct srtla_stats_filter));
	filter->context = context;
	
	const char *port_str = obs_data_get_string(settings, "listen_port");
	if (port_str && *port_str) {
		filter->listen_port = atoi(port_str);
	} else {
		filter->listen_port = (int)obs_data_get_int(settings, "listen_port");
	}
	
	filter->last_bytes = 0;
	filter->last_time_ms = os_gettime_ns() / 1000000;
	
	return filter;
}

static void srtla_stats_filter_destroy(void *data)
{
	struct srtla_stats_filter *filter = data;
	bfree(filter);
}

static void srtla_stats_filter_update(void *data, obs_data_t *settings)
{
	struct srtla_stats_filter *filter = data;
	const char *port_str = obs_data_get_string(settings, "listen_port");
	if (port_str && *port_str) {
		filter->listen_port = atoi(port_str);
	}
}

static obs_properties_t *srtla_stats_filter_get_properties(void *data)
{
	(void)data;
	obs_properties_t *props = obs_properties_create();
	obs_properties_set_flags(props, OBS_PROPERTIES_DEFER_UPDATE);

	obs_property_t *p = obs_properties_add_list(props, "listen_port", "SRTLA Receiver", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	srtla_populate_receivers_list(p);

	return props;
}

static void srtla_stats_filter_video_tick(void *data, float seconds)
{
	(void)seconds;
	struct srtla_stats_filter *filter = data;
	if (filter->listen_port <= 0) return;

	uint64_t current_time_ms = os_gettime_ns() / 1000000;
	if (current_time_ms - filter->last_time_ms >= 1000) {
		uint64_t current_bytes = srtla_get_total_bytes(filter->listen_port);
		
		if (filter->last_bytes > 0 && current_bytes >= filter->last_bytes) {
			uint64_t bytes_diff = current_bytes - filter->last_bytes;
			uint64_t time_diff_ms = current_time_ms - filter->last_time_ms;
			
			// KBPS calculation (kilobits per second)
			double kbps = (double)bytes_diff * 8.0 / (double)time_diff_ms;
			
			char text[64];
			snprintf(text, sizeof(text), "%.1f KBPS", kbps);
			
			obs_source_t *parent = obs_filter_get_parent(filter->context);
			if (parent) {
				obs_data_t *settings = obs_source_get_settings(parent);
				obs_data_set_string(settings, "text", text);
				obs_source_update(parent, settings);
				obs_data_release(settings);
			}
		} else if (current_bytes < filter->last_bytes || current_bytes == 0) {
			// Handle restarts/disconnects
			obs_source_t *parent = obs_filter_get_parent(filter->context);
			if (parent) {
				obs_data_t *settings = obs_source_get_settings(parent);
				obs_data_set_string(settings, "text", "0.0 KBPS");
				obs_source_update(parent, settings);
				obs_data_release(settings);
			}
		}
		
		filter->last_bytes = current_bytes;
		filter->last_time_ms = current_time_ms;
	}
}

struct obs_source_info srtla_stats_filter_info = {
	.id = "srtla_stats_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = srtla_stats_filter_get_name,
	.create = srtla_stats_filter_create,
	.destroy = srtla_stats_filter_destroy,
	.update = srtla_stats_filter_update,
	.get_properties = srtla_stats_filter_get_properties,
	.video_tick = srtla_stats_filter_video_tick,
};
