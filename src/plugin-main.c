/*
Plugin Name
Copyright (C) <Year> <Developer> <Email Address>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include <obs-module.h>
#include <plugin-support.h>
#include <obs-frontend-api.h>

#ifdef _WIN32
#include <windows.h>
#endif

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

extern struct obs_source_info srtla_source_info;

extern void *create_srtla_dock();
extern void *create_srtla_multistream_dock();
extern void setup_srtla_menu();

char *srtla_get_frpc_path(void)
{
#ifdef _WIN32
	return obs_module_file("frpc.exe");
#else
	return obs_module_file("frpc");
#endif
}

static void frontend_event_cb(enum obs_frontend_event event, void *private_data)
{
	(void)private_data;
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
		setup_srtla_menu();
		obs_frontend_add_custom_qdock("srtla_status_dock", create_srtla_dock());
		obs_frontend_add_custom_qdock("srtla_multistream_dock", create_srtla_multistream_dock());
	}
}

extern struct obs_source_info srtla_stats_filter_info;

static void register_source_compat(struct obs_source_info *info)
{
	uint32_t host_version = obs_get_version();
	size_t info_size = sizeof(struct obs_source_info);

	if (host_version < MAKE_SEMANTIC_VERSION(31, 0, 0) && info_size > 408) {
		info_size = 408;
	} else if (host_version < MAKE_SEMANTIC_VERSION(30, 0, 0) && info_size > 368) {
		info_size = 368;
	}

	obs_register_source_s(info, info_size);
}

bool obs_module_load(void)
{
	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	register_source_compat(&srtla_source_info);
	register_source_compat(&srtla_stats_filter_info);
	obs_frontend_add_event_callback(frontend_event_cb, NULL);
	return true;
}

void obs_module_unload(void)
{
	obs_log(LOG_INFO, "plugin unloaded");
}
