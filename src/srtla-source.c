#include <obs-module.h>
#include <plugin-support.h>
#include <util/threading.h>
#include <util/bmem.h>
#include <util/platform.h>

#ifdef _WIN32
#include <windows.h>
#endif

extern int srtla_rec_main(const char *listen_ip, int listen_port, const char *srt_host, int srt_port, volatile int *stop_flag);

struct srtla_source {
	obs_source_t *source;
	obs_source_t *media_source;
	
	int listen_port;
	int local_srt_port;
	char *listen_ip;
	
	pthread_t srtla_thread;
	volatile int stop_flag;
	bool thread_running;
	
	struct srtla_source *next;
};

static struct srtla_source *sources_head = NULL;
static pthread_mutex_t sources_mutex = PTHREAD_MUTEX_INITIALIZER;

static const char *srtla_source_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return "SRTLA Receiver";
}

static void *srtla_source_create(obs_data_t *settings, obs_source_t *source)
{
	struct srtla_source *context = bzalloc(sizeof(struct srtla_source));
	context->source = source;
	context->thread_running = false;
	
	pthread_mutex_lock(&sources_mutex);
	context->next = sources_head;
	sources_head = context;
	pthread_mutex_unlock(&sources_mutex);
	
	return context;
}

static void srtla_stop_thread(struct srtla_source *context)
{
	if (context->thread_running) {
		context->stop_flag = 1;
		pthread_join(context->srtla_thread, NULL);
		context->thread_running = false;
	}
}

static void srtla_source_destroy(void *data)
{
	struct srtla_source *context = data;
	srtla_stop_thread(context);
	
	if (context->media_source) {
		obs_source_release(context->media_source);
	}
	
	pthread_mutex_lock(&sources_mutex);
	struct srtla_source **curr = &sources_head;
	while (*curr) {
		if (*curr == context) {
			*curr = context->next;
			break;
		}
		curr = &(*curr)->next;
	}
	pthread_mutex_unlock(&sources_mutex);
	
	bfree(context->listen_ip);
	bfree(context);
}

static void *srtla_thread_func(void *data)
{
	struct srtla_source *context = data;
	
	// Wait a bit to ensure media source is listening
	os_sleep_ms(500);
	
	obs_log(LOG_INFO, "[SRTLA] Starting srtla_rec thread on IP %s, port %d, proxying to 127.0.0.1:%d", 
			context->listen_ip ? context->listen_ip : "ANY", context->listen_port, context->local_srt_port);
			
	srtla_rec_main(context->listen_ip, context->listen_port, "127.0.0.1", context->local_srt_port, &context->stop_flag);
	
	obs_log(LOG_INFO, "[SRTLA] srtla_rec thread exited");
	return NULL;
}

static void srtla_source_update(void *data, obs_data_t *settings)
{
#ifdef _WIN32
	__try {
#endif
	struct srtla_source *context = data;
	
	int new_listen_port = (int)obs_data_get_int(settings, "listen_port");
	int new_local_srt_port = (int)obs_data_get_int(settings, "local_srt_port");
	const char *new_listen_ip = obs_data_get_string(settings, "listen_ip");
	
	// Auto-resolve port conflicts for new sources
	if (context->listen_port == 0) {
		if (new_listen_port == 0) new_listen_port = 5000;
		if (new_local_srt_port == 0) new_local_srt_port = 4000;
		
		while (true) {
			bool conflict = false;
			pthread_mutex_lock(&sources_mutex);
			for (struct srtla_source *s = sources_head; s; s = s->next) {
				if (s != context && s->listen_port == new_listen_port) {
					conflict = true;
					break;
				}
			}
			pthread_mutex_unlock(&sources_mutex);
			if (!conflict) break;
			new_listen_port++;
		}
		obs_data_set_int(settings, "listen_port", new_listen_port);

		while (true) {
			bool conflict = false;
			pthread_mutex_lock(&sources_mutex);
			for (struct srtla_source *s = sources_head; s; s = s->next) {
				if (s != context && s->local_srt_port == new_local_srt_port) {
					conflict = true;
					break;
				}
			}
			pthread_mutex_unlock(&sources_mutex);
			if (!conflict) break;
			new_local_srt_port++;
		}
		obs_data_set_int(settings, "local_srt_port", new_local_srt_port);
	} else {
		// Prevent user from changing to an already occupied port
		bool conflict = false;
		pthread_mutex_lock(&sources_mutex);
		for (struct srtla_source *s = sources_head; s; s = s->next) {
			if (s != context && s->listen_port == new_listen_port) {
				conflict = true;
				break;
			}
		}
		pthread_mutex_unlock(&sources_mutex);
		
		if (conflict) {
			obs_log(LOG_WARNING, "[SRTLA] Port %d is already in use by another SRTLA source! Reverting.", new_listen_port);
			obs_data_set_int(settings, "listen_port", context->listen_port);
			new_listen_port = context->listen_port; // prevent restart
		}
	}
	
	bool listen_ip_changed = false;
	if (new_listen_ip) {
		if (!context->listen_ip || strcmp(context->listen_ip, new_listen_ip) != 0) {
			listen_ip_changed = true;
		}
	} else if (context->listen_ip) {
		listen_ip_changed = true;
	}

	bool media_restart_needed = (!context->media_source || context->local_srt_port != new_local_srt_port);
	bool thread_restart_needed = (context->listen_port != new_listen_port || context->local_srt_port != new_local_srt_port || listen_ip_changed || !context->thread_running);
	
	if (thread_restart_needed || media_restart_needed) {
		srtla_stop_thread(context);
		
		context->listen_port = new_listen_port;
		context->local_srt_port = new_local_srt_port;
		
		bfree(context->listen_ip);
		context->listen_ip = new_listen_ip ? bstrdup(new_listen_ip) : NULL;
		
		if (media_restart_needed) {
			if (context->media_source) {
				obs_source_release(context->media_source);
				context->media_source = NULL;
			}
			
			char url[256];
			snprintf(url, sizeof(url), "srt://127.0.0.1:%d?mode=listener", context->local_srt_port);
			
			obs_data_t *media_settings = obs_data_create();
			obs_data_set_string(media_settings, "input", url);
			obs_data_set_bool(media_settings, "is_local_file", false);
			obs_data_set_bool(media_settings, "hw_decode", true);
			obs_data_set_bool(media_settings, "clear_on_media_end", false);
			obs_data_set_bool(media_settings, "restart_on_activate", true);
			obs_data_set_int(media_settings, "reconnect_delay_sec", 1);
			
			char source_name[256];
			snprintf(source_name, sizeof(source_name), "SRTLA_Internal_%d", context->listen_port);
			context->media_source = obs_source_create("ffmpeg_source", source_name, media_settings, NULL);
			obs_data_release(media_settings);
			
			if (context->media_source) {
				obs_source_add_active_child(context->source, context->media_source);
			} else {
				obs_log(LOG_ERROR, "[SRTLA] Failed to create internal ffmpeg_source");
			}
		}
		
		context->stop_flag = 0;
		if (pthread_create(&context->srtla_thread, NULL, srtla_thread_func, context) == 0) {
			context->thread_running = true;
		}
	}
#ifdef _WIN32
	} __except(EXCEPTION_EXECUTE_HANDLER) {
		obs_log(LOG_ERROR, "[SRTLA] SEH Exception caught in srtla_source_update! OBS crash prevented.");
	}
#endif
}

static void srtla_source_video_render(void *data, gs_effect_t *effect)
{
	struct srtla_source *context = data;
	if (context->media_source) {
		obs_source_video_render(context->media_source);
	}
}

static uint32_t srtla_source_get_width(void *data)
{
	struct srtla_source *context = data;
	return context->media_source ? obs_source_get_base_width(context->media_source) : 0;
}

static uint32_t srtla_source_get_height(void *data)
{
	struct srtla_source *context = data;
	return context->media_source ? obs_source_get_base_height(context->media_source) : 0;
}

static obs_properties_t *srtla_source_get_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();
	obs_properties_set_flags(props, OBS_PROPERTIES_DEFER_UPDATE);
	
	obs_properties_add_text(props, "listen_ip", "SRTLA Bind IP (empty for ANY)", OBS_TEXT_DEFAULT);
	obs_properties_add_int(props, "listen_port", "SRTLA Listen Port (UDP)", 1, 65535, 1);
	obs_properties_add_int(props, "local_srt_port", "Local SRT Port", 1, 65535, 1);
	
	return props;
}

static void srtla_source_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "listen_ip", "");
	obs_data_set_default_int(settings, "listen_port", 5000);
	obs_data_set_default_int(settings, "local_srt_port", 4000);
}

struct obs_source_info srtla_source_info = {
	.id = "srtla_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_ASYNC | OBS_SOURCE_CUSTOM_DRAW,
	.get_name = srtla_source_get_name,
	.create = srtla_source_create,
	.destroy = srtla_source_destroy,
	.update = srtla_source_update,
	.get_properties = srtla_source_get_properties,
	.get_defaults = srtla_source_get_defaults,
	.video_render = srtla_source_video_render,
	.get_width = srtla_source_get_width,
	.get_height = srtla_source_get_height,
};

void srtla_force_stop(void *data) {
#ifdef _WIN32
	__try {
#endif
	struct srtla_source *context = data;
	srtla_stop_thread(context);
	if (context->media_source) {
		obs_source_release(context->media_source);
		context->media_source = NULL;
	}
#ifdef _WIN32
	} __except(EXCEPTION_EXECUTE_HANDLER) {
		obs_log(LOG_ERROR, "[SRTLA] SEH Exception caught in srtla_force_stop! OBS crash prevented.");
	}
#endif
}

void srtla_force_start(void *data) {
	struct srtla_source *context = data;
	obs_data_t *settings = obs_source_get_settings(context->source);
	srtla_source_update(context, settings);
	obs_data_release(settings);
}

void srtla_force_stop_all() {
	struct srtla_source **targets = NULL;
	int count = 0;
	pthread_mutex_lock(&sources_mutex);
	for (struct srtla_source *s = sources_head; s; s = s->next) count++;
	if (count > 0) {
		targets = calloc(count, sizeof(struct srtla_source *));
		int i = 0;
		for (struct srtla_source *s = sources_head; s; s = s->next) {
			targets[i++] = s;
		}
	}
	pthread_mutex_unlock(&sources_mutex);

	for (int i = 0; i < count; i++) {
		srtla_force_stop(targets[i]);
	}
	free(targets);
}

void srtla_force_start_all() {
	struct srtla_source **targets = NULL;
	int count = 0;
	pthread_mutex_lock(&sources_mutex);
	for (struct srtla_source *s = sources_head; s; s = s->next) count++;
	if (count > 0) {
		targets = calloc(count, sizeof(struct srtla_source *));
		int i = 0;
		for (struct srtla_source *s = sources_head; s; s = s->next) {
			targets[i++] = s;
		}
	}
	pthread_mutex_unlock(&sources_mutex);

	for (int i = 0; i < count; i++) {
		srtla_force_start(targets[i]);
	}
	free(targets);
}

void srtla_force_restart_all() {
	struct srtla_source **targets = NULL;
	int count = 0;
	pthread_mutex_lock(&sources_mutex);
	for (struct srtla_source *s = sources_head; s; s = s->next) count++;
	if (count > 0) {
		targets = calloc(count, sizeof(struct srtla_source *));
		int i = 0;
		for (struct srtla_source *s = sources_head; s; s = s->next) {
			targets[i++] = s;
		}
	}
	pthread_mutex_unlock(&sources_mutex);

	for (int i = 0; i < count; i++) {
		srtla_force_stop(targets[i]);
		srtla_force_start(targets[i]);
	}
	free(targets);
}

void srtla_get_all_receivers_json(char *out_buffer, int max_len) {
	int offset = 0;
	if (out_buffer && max_len > 0) {
		offset += snprintf(out_buffer + offset, max_len - offset, "[");
		bool first = true;
		pthread_mutex_lock(&sources_mutex);
		for (struct srtla_source *s = sources_head; s; s = s->next) {
			if (!first) offset += snprintf(out_buffer + offset, max_len - offset, ",");
			first = false;
			const char *name = obs_source_get_name(s->source);
			offset += snprintf(out_buffer + offset, max_len - offset, 
				"{\"name\":\"%s\",\"listen_port\":%d,\"running\":%s}",
				name ? name : "Unknown",
				s->listen_port,
				s->thread_running ? "true" : "false"
			);
		}
		pthread_mutex_unlock(&sources_mutex);
		snprintf(out_buffer + offset, max_len - offset, "]");
	}
}

void srtla_force_start_by_name(const char *name) {
	if (!name) return;
	struct srtla_source *target = NULL;
	pthread_mutex_lock(&sources_mutex);
	for (struct srtla_source *s = sources_head; s; s = s->next) {
		const char *s_name = obs_source_get_name(s->source);
		if (s_name && strcmp(s_name, name) == 0) {
			target = s;
			break;
		}
	}
	pthread_mutex_unlock(&sources_mutex);

	if (target) {
		srtla_force_start(target);
	}
}

void srtla_force_stop_by_name(const char *name) {
	if (!name) return;
	struct srtla_source *target = NULL;
	pthread_mutex_lock(&sources_mutex);
	for (struct srtla_source *s = sources_head; s; s = s->next) {
		const char *s_name = obs_source_get_name(s->source);
		if (s_name && strcmp(s_name, name) == 0) {
			target = s;
			break;
		}
	}
	pthread_mutex_unlock(&sources_mutex);

	if (target) {
		srtla_force_stop(target);
	}
}

void srtla_force_restart_by_name(const char *name) {
	if (!name) return;
	struct srtla_source *target = NULL;
	pthread_mutex_lock(&sources_mutex);
	for (struct srtla_source *s = sources_head; s; s = s->next) {
		const char *s_name = obs_source_get_name(s->source);
		if (s_name && strcmp(s_name, name) == 0) {
			target = s;
			break;
		}
	}
	pthread_mutex_unlock(&sources_mutex);

	if (target) {
		srtla_force_stop(target);
		srtla_force_start(target);
	}
}
