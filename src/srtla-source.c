#include <obs-module.h>
#include <plugin-support.h>
#include <util/threading.h>
#include <util/bmem.h>
#include <util/platform.h>
#include <util/dstr.h>

#include <obs-frontend-api.h>
#include <media-io/audio-io.h>

#include "compat_pthread.h"
#include "rist_rec.h"

#ifdef _WIN32
#include <windows.h>
#endif
#include <math.h>

extern int srtla_rec_main(const char *listen_ip, int listen_port, const char *srt_host, int srt_port,
			  volatile int *stop_flag);
extern int srtla_get_group_count_by_port(int listen_port);
extern void srtla_reset_group_by_port(int listen_port);

enum srtla_recovery_action {
	RECOVERY_NONE = 0,
	RECOVERY_RELOAD,
	RECOVERY_RESTART
};

enum receiver_protocol {
	PROTOCOL_SRTLA = 0,
	PROTOCOL_RIST = 1
};

struct srtla_source {
	obs_source_t *source;
	obs_source_t *media_source;

	enum receiver_protocol protocol;

	int listen_port;
	int local_srt_port;
	char *listen_ip;
	char *playback_engine;

	int rist_profile;
	int rist_buffer_ms;
	char *rist_passphrase;
	int rist_key_size;
	int rist_stream_id;

	pthread_t srtla_thread;
	volatile int stop_flag;
	bool thread_running;
	bool was_connected;
	
	enum srtla_recovery_action pending_recovery;
	
	uint64_t last_audio_ts;
	uint64_t last_original_ts;
	
	uint64_t last_audio_time;
	uint64_t last_video_time;
	uint64_t connected_since;
	uint64_t starved_since;
	uint64_t last_recovery_time;
	int recovery_attempts;
	double current_stream_hz;
	uint64_t ts_window_start;
	uint64_t frames_in_window;
	bool needs_reload;
	int64_t current_audio_drift;
	float current_audio_db;
	int auto_reset_count;

	struct srtla_source *next;
};

static struct srtla_source *sources_head = NULL;
static pthread_mutex_t sources_mutex = PTHREAD_MUTEX_INITIALIZER;

#ifdef _WIN32
__declspec(dllexport)
#endif
bool srtla_is_audio_starved(int listen_port) {
	pthread_mutex_lock(&sources_mutex);
	struct srtla_source *curr = sources_head;
	bool starved = false;
	while (curr) {
		if (curr->listen_port == listen_port) {
			int groups = 0;
			if (curr->protocol == PROTOCOL_RIST) {
				groups = rist_get_peer_count_by_port(curr->listen_port);
			} else {
				groups = srtla_get_group_count_by_port(curr->listen_port);
			}
			if (groups > 0) {
				uint64_t now = os_gettime_ns();
				// Only flag starved if video is active (received in the last 2s) and:
				// 1. connection is >12s old with zero audio ever received, or
				// 2. audio was active and completely stopped for >= 8s, or
				// 3. audio is severely degraded (< 30kHz).
				bool active_video = (curr->last_video_time > 0 && (now - curr->last_video_time < 2000000000ULL));
				if (active_video) {
					if (curr->last_audio_time == 0) {
						if (curr->connected_since > 0 && (now - curr->connected_since >= 12000000000ULL)) {
							starved = true;
						}
					} else if (now - curr->last_audio_time >= 8000000000ULL) {
						starved = true;
					} else if (curr->current_stream_hz > 0 && curr->current_stream_hz < 30000.0) {
						starved = true;
					}
				}
			}
			break;
		}
		curr = curr->next;
	}
	pthread_mutex_unlock(&sources_mutex);
	return starved;
}

#ifdef _WIN32
__declspec(dllexport)
#endif
bool srtla_is_any_media_playing() {
	pthread_mutex_lock(&sources_mutex);
	struct srtla_source *curr = sources_head;
	bool playing = false;
	uint64_t now = os_gettime_ns();
	while (curr) {
		if (curr->last_video_time > 0 && (now - curr->last_video_time < 1000000000ULL)) {
			playing = true;
			break;
		}
		curr = curr->next;
	}
	pthread_mutex_unlock(&sources_mutex);
	return playing;
}

static const char *srtla_source_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return "SRTLA Receiver";
}

static void srtla_audio_capture_cb(void *param, obs_source_t *source, const struct audio_data *audio_data, bool muted)
{
	UNUSED_PARAMETER(source);
	struct srtla_source *context = param;
	if (!context || !audio_data) return;

	uint64_t now = os_gettime_ns();
	context->last_audio_time = now;
	context->recovery_attempts = 0;

	if (muted) return;

	struct obs_audio_info aoi;
	if (!obs_get_audio_info(&aoi)) return;

	struct obs_source_audio out = {0};
	out.speakers = aoi.speakers;
	out.samples_per_sec = aoi.samples_per_sec;
	out.format = AUDIO_FORMAT_FLOAT_PLANAR;
	for (int i = 0; i < MAX_AV_PLANES; i++) {
		out.data[i] = audio_data->data[i];
	}
	out.frames = audio_data->frames;

	float max_peak = 1e-9f;
	for (int c = 0; c < (int)out.speakers; c++) {
		if (out.data[c]) {
			float *ch_data = (float *)out.data[c];
			for (int i = 0; i < (int)out.frames; i++) {
				float val = fabsf(ch_data[i]);
				if (val > max_peak) max_peak = val;
			}
		}
	}
	context->current_audio_db = 20.0f * log10f(max_peak);

	// Monotonic sample-accurate timestamp smoothing to eliminate jitter-induced drops in OBS
	uint64_t sample_rate = (out.samples_per_sec > 0) ? (uint64_t)out.samples_per_sec : 48000ULL;
	uint64_t frame_duration_ns = ((uint64_t)out.frames * 1000000000ULL) / sample_rate;

	if (context->last_audio_ts == 0) {
		context->last_audio_ts = audio_data->timestamp;
		context->last_original_ts = audio_data->timestamp;
	} else {
		uint64_t expected_ts = context->last_audio_ts + frame_duration_ns;
		int64_t drift = (int64_t)audio_data->timestamp - (int64_t)expected_ts;
		context->current_audio_drift = drift;
		
		// If drift is within +/-50ms, gently nudge towards source clock without causing pitch or audio drop glitches
		if (drift > -50000000LL && drift < 50000000LL) {
			expected_ts += (drift / 32);
		} else {
			if (drift > 1000000000LL || drift < -1000000000LL) {
				obs_log(LOG_INFO, "[SRTLA] Massive PTS discontinuity detected (drift: %.2f sec). Scheduling internal media player reload to resync.", (double)drift / 1000000000.0);
				context->needs_reload = true;
			}
			// Large PTS discontinuity / resync (e.g. stream reconnect): jump directly to new timestamp
			expected_ts = audio_data->timestamp;
		}
		context->last_audio_ts = expected_ts;
	}
	out.timestamp = context->last_audio_ts;

	// --- Passive Audio Monitor Algorithm ---
	if (context->ts_window_start == 0 || now - context->ts_window_start >= 2000000000ULL) {
		if (context->ts_window_start != 0) {
			context->current_stream_hz = (double)context->frames_in_window / ((double)(now - context->ts_window_start) / 1000000000.0);
		}
		context->ts_window_start = now;
		context->frames_in_window = 0;
	}
	context->frames_in_window += out.frames;

	// Diagnostic logging to verify monotonic timeline and jitter
	static uint64_t last_log_time = 0;
	static uint64_t last_ts = 0;
	static int call_count = 0;
	static int total_frames = 0;
	static uint64_t min_gap = (uint64_t)-1;
	static uint64_t max_gap = 0;
	
	call_count++;
	total_frames += out.frames;
	uint64_t ts_gap = (last_ts == 0) ? 0 : (out.timestamp - last_ts);
	last_ts = out.timestamp;

	if (ts_gap > 0) {
		if (ts_gap < min_gap) min_gap = ts_gap;
		if (ts_gap > max_gap) max_gap = ts_gap;
	}

	if (now - last_log_time > 1000000000ULL) {
		double computed_hz = (last_log_time > 0) ? ((double)total_frames / (double)(now - last_log_time) * 1000000000.0) : 0;
		obs_log(LOG_INFO, "[SRTLA Audio Diagnostic] 1s Stats | calls: %d | total_frames: %d | min_gap: %.2f ms | max_gap: %.2f ms | stream_hz: %.1f | obs_hz: %lu | current_ts: %llu",
			call_count, total_frames, 
			(min_gap == (uint64_t)-1) ? 0 : (double)min_gap / 1000000.0, 
			(double)max_gap / 1000000.0,
			computed_hz,
			(unsigned long)out.samples_per_sec,
			out.timestamp);
		
		call_count = 0;
		total_frames = 0;
		min_gap = (uint64_t)-1;
		max_gap = 0;
		last_log_time = now;
	}

	obs_source_output_audio(context->source, &out);
}

static void *srtla_source_create(obs_data_t *settings, obs_source_t *source)
{
	struct srtla_source *context = bzalloc(sizeof(struct srtla_source));
	context->source = source;
	context->thread_running = false;
	
	if (settings) {
		const char *proto = obs_data_get_string(settings, "protocol");
		context->protocol = (proto && strcmp(proto, "rist") == 0) ? PROTOCOL_RIST : PROTOCOL_SRTLA;
		context->listen_port = (int)obs_data_get_int(settings, "listen_port");
	}

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

static void srtla_destroy_media_source(struct srtla_source *context)
{
	if (context->media_source) {
		obs_source_remove_audio_capture_callback(context->media_source, srtla_audio_capture_cb, context);
		obs_source_dec_active(context->media_source);
		obs_source_release(context->media_source);
		context->media_source = NULL;
	}
	context->last_audio_ts = 0;
	context->last_original_ts = 0;
	context->last_audio_time = 0;
	context->current_stream_hz = 0;
	context->starved_since = 0;
	context->ts_window_start = 0;
	context->frames_in_window = 0;
}

static void srtla_create_media_source(struct srtla_source *context)
{
	if (context->media_source) {
		srtla_destroy_media_source(context);
	}

	char url[256];
	if (context->protocol == PROTOCOL_RIST) {
		snprintf(url, sizeof(url), "udp://127.0.0.1:%d?pkt_size=1316&buffer_size=8388608&fifo_size=500000", context->local_srt_port);
	} else {
		snprintf(url, sizeof(url), "srt://127.0.0.1:%d?mode=listener", context->local_srt_port);
	}

	char source_name[256];
	const char *parent_name = obs_source_get_name(context->source);
	snprintf(source_name, sizeof(source_name), "%s_Internal", parent_name ? parent_name : "SRTLA");

	bool use_vlc = (context->playback_engine && strcmp(context->playback_engine, "vlc") == 0);

	if (use_vlc) {
		obs_data_t *vlc_settings = obs_data_create();
		obs_data_array_t *playlist = obs_data_array_create();
		obs_data_t *item = obs_data_create();
		
		char vlc_url[256];
		if (context->protocol == PROTOCOL_RIST) {
			snprintf(vlc_url, sizeof(vlc_url), "udp://@127.0.0.1:%d", context->local_srt_port);
		} else {
			strncpy(vlc_url, url, sizeof(vlc_url));
		}
		
		obs_data_set_string(item, "value", vlc_url);
		obs_data_array_push_back(playlist, item);
		obs_data_release(item);

		obs_data_set_array(vlc_settings, "playlist", playlist);
		obs_data_array_release(playlist);

		obs_data_set_bool(vlc_settings, "loop", false);
		obs_data_set_bool(vlc_settings, "shuffle", false);
		obs_data_set_string(vlc_settings, "playback_behavior", "always_play");
		obs_data_set_int(vlc_settings, "network_caching", 500);
		obs_data_set_int(vlc_settings, "track", 1);
		obs_data_set_int(vlc_settings, "subtitle_track", 1);
		obs_data_set_bool(vlc_settings, "subtitle_enable", false);

		context->media_source = obs_source_create_private("vlc_source", source_name, vlc_settings);
		obs_data_release(vlc_settings);

		if (!context->media_source) {
			obs_log(LOG_WARNING, "[SRTLA] VLC Video Source creation failed (is 64-bit VLC installed?). Falling back to FFmpeg.");
			use_vlc = false;
		} else {
			obs_log(LOG_INFO, "[SRTLA] Created internal vlc_source successfully (low-latency network caching 500ms)");
		}
	}

	if (!use_vlc) {
		obs_data_t *media_settings = obs_data_create();
		obs_data_set_string(media_settings, "input", url);
		obs_data_set_bool(media_settings, "is_local_file", false);
		obs_data_set_bool(media_settings, "hw_decode", true);
		obs_data_set_bool(media_settings, "clear_on_media_end", false);
		obs_data_set_bool(media_settings, "restart_on_activate", false);
		obs_data_set_bool(media_settings, "close_when_inactive", false);
		obs_data_set_int(media_settings, "reconnect_delay_sec", 1);
		obs_data_set_int(media_settings, "buffering_mb", 0);
		obs_data_set_string(media_settings, "ffmpeg_options",
				    "fflags=nobuffer+discardcorrupt+genpts probesize=131072 analyzeduration=1000000");

		context->media_source = obs_source_create_private("ffmpeg_source", source_name, media_settings);
		obs_data_release(media_settings);

		if (context->media_source) {
			obs_log(LOG_INFO, "[SRTLA] Created internal ffmpeg_source successfully (live continuous playback)");
		} else {
			obs_log(LOG_ERROR, "[SRTLA] Failed to create internal ffmpeg_source");
		}
	}

	if (context->media_source) {
		obs_source_set_async_decoupled(context->media_source, true);
		obs_source_set_async_unbuffered(context->media_source, true);
		obs_source_set_audio_mixers(context->media_source, 0);
		obs_source_set_async_decoupled(context->source, true);
		obs_source_add_audio_capture_callback(context->media_source, srtla_audio_capture_cb, context);

		// Keep media source permanently active in background so stream continues receiving across scenes
		obs_source_inc_active(context->media_source);
	}
	context->starved_since = 0;
	context->last_recovery_time = os_gettime_ns();
}

void srtla_reload_media_source(void *data)
{
#ifdef _WIN32
	__try {
#endif
		struct srtla_source *context = data;
		if (!context) return;
		obs_log(LOG_INFO, "[SRTLA] Reloading internal media player for port %d to resync stream...", context->listen_port);
		
		// Ensure we destroy the old media source before creating a new one to prevent memory leaks and crashes
		srtla_destroy_media_source(context);
		srtla_create_media_source(context);
#ifdef _WIN32
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		obs_log(LOG_ERROR, "[SRTLA] SEH Exception caught in srtla_reload_media_source! OBS crash prevented.");
	}
#endif
}

static void srtla_source_destroy(void *data)
{
	struct srtla_source *context = data;
	srtla_stop_thread(context);
	srtla_destroy_media_source(context);

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

	bfree(context->playback_engine);
	bfree(context->listen_ip);
	bfree(context->rist_passphrase);
	bfree(context);
}

static void *srtla_thread_func(void *data)
{
	struct srtla_source *context = data;

	// Wait a bit to ensure media source is listening
	os_sleep_ms(500);

	if (context->protocol == PROTOCOL_SRTLA) {
		obs_log(LOG_INFO, "[SRTLA] Starting srtla_rec thread on IP %s, port %d, proxying to 127.0.0.1:%d",
			context->listen_ip ? context->listen_ip : "ANY", context->listen_port, context->local_srt_port);

		srtla_rec_main(context->listen_ip, context->listen_port, "127.0.0.1", context->local_srt_port,
			       &context->stop_flag);
	} else if (context->protocol == PROTOCOL_RIST) {
		obs_log(LOG_INFO, "[RIST] Starting RIST receiver on IP %s, port %d", 
			context->listen_ip ? context->listen_ip : "ANY", context->listen_port);
			
		struct rist_config r_cfg = {0};
		if (context->listen_ip) {
			strncpy(r_cfg.listen_ip, context->listen_ip, sizeof(r_cfg.listen_ip) - 1);
		}
		r_cfg.listen_port = context->listen_port;
		r_cfg.local_srt_port = context->local_srt_port;
		r_cfg.stop_flag = &context->stop_flag;
		r_cfg.profile = context->rist_profile;
		r_cfg.buffer_ms = context->rist_buffer_ms;
		if (context->rist_passphrase) {
			strncpy(r_cfg.passphrase, context->rist_passphrase, sizeof(r_cfg.passphrase) - 1);
		}
		r_cfg.key_size = context->rist_key_size;
		r_cfg.stream_id = context->rist_stream_id;

		rist_rec_main(&r_cfg);
	}

	obs_log(LOG_INFO, "[SRTLA/RIST] receiver thread exited");
	return NULL;
}

static void srtla_source_update(void *data, obs_data_t *settings)
{
#ifdef _WIN32
	__try {
#endif
		struct srtla_source *context = data;

		const char *new_proto_str = obs_data_get_string(settings, "protocol");
		enum receiver_protocol new_proto = (new_proto_str && strcmp(new_proto_str, "rist") == 0) ? PROTOCOL_RIST : PROTOCOL_SRTLA;

		long long new_listen_port = obs_data_get_int(settings, "listen_port");
		long long new_local_srt_port = obs_data_get_int(settings, "local_srt_port");
		const char *new_listen_ip = obs_data_get_string(settings, "listen_ip");

		// Auto-resolve port conflicts for new sources
		if (context->listen_port == 0) {
			if (new_listen_port == 0)
				new_listen_port = 5000;
			if (new_local_srt_port == 0)
				new_local_srt_port = 4000;

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
				if (!conflict)
					break;
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
				if (!conflict)
					break;
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
				obs_log(LOG_WARNING,
					"[SRTLA] Port %d is already in use by another SRTLA source! Reverting.",
					new_listen_port);
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

		const char *new_engine = obs_data_get_string(settings, "playback_engine");
		if (!new_engine || !*new_engine) new_engine = "ffmpeg";
		bool engine_changed = (!context->playback_engine || strcmp(context->playback_engine, new_engine) != 0);
		if (engine_changed) {
			bfree(context->playback_engine);
			context->playback_engine = bstrdup(new_engine);
		}

		const char *new_passphrase = obs_data_get_string(settings, "rist_passphrase");
		bool config_changed = false;
		if (context->protocol != new_proto) config_changed = true;
		if (context->rist_profile != obs_data_get_int(settings, "rist_profile")) config_changed = true;
		if (context->rist_buffer_ms != obs_data_get_int(settings, "rist_buffer_ms")) config_changed = true;
		if (context->rist_key_size != obs_data_get_int(settings, "rist_key_size")) config_changed = true;
		if (context->rist_stream_id != obs_data_get_int(settings, "rist_stream_id")) config_changed = true;
		if (new_passphrase && (!context->rist_passphrase || strcmp(context->rist_passphrase, new_passphrase) != 0)) config_changed = true;
		else if (!new_passphrase && context->rist_passphrase) config_changed = true;

		bool media_restart_needed = (!context->media_source || context->local_srt_port != new_local_srt_port || engine_changed || context->protocol != new_proto);
		bool thread_restart_needed = (context->listen_port != new_listen_port ||
					      context->local_srt_port != new_local_srt_port || listen_ip_changed || config_changed ||
					      !context->thread_running);

		if (thread_restart_needed || media_restart_needed) {
			srtla_stop_thread(context);
			context->auto_reset_count = 0;

			context->protocol = new_proto;
			context->rist_profile = (int)obs_data_get_int(settings, "rist_profile");
			context->rist_buffer_ms = (int)obs_data_get_int(settings, "rist_buffer_ms");
			context->rist_key_size = (int)obs_data_get_int(settings, "rist_key_size");
			context->rist_stream_id = (int)obs_data_get_int(settings, "rist_stream_id");
			
			if (context->rist_passphrase) bfree(context->rist_passphrase);
			context->rist_passphrase = new_passphrase ? bstrdup(new_passphrase) : NULL;

			context->listen_port = (int)new_listen_port;
			context->local_srt_port = (int)new_local_srt_port;

			bfree(context->listen_ip);
			context->listen_ip = new_listen_ip ? bstrdup(new_listen_ip) : NULL;

			if (media_restart_needed) {
				// Destroy any existing media source to prevent thread leaks which crash OBS on exit
				srtla_destroy_media_source(context);
				srtla_create_media_source(context);
			}

			context->stop_flag = 0;
			if (pthread_create(&context->srtla_thread, NULL, srtla_thread_func, context) == 0) {
				context->thread_running = true;
			}
		}
#ifdef _WIN32
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		obs_log(LOG_ERROR, "[SRTLA] SEH Exception caught in srtla_source_update! OBS crash prevented.");
	}
#endif
}

static void srtla_source_video_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct srtla_source *context = data;
	
	if (context->media_source) {
		obs_source_video_render(context->media_source);
		if (obs_source_get_base_width(context->media_source) > 0) {
			context->last_video_time = os_gettime_ns();
		}
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

static void srtla_source_activate(void *data)
{
	UNUSED_PARAMETER(data);
}

static void srtla_source_deactivate(void *data)
{
	UNUSED_PARAMETER(data);
}

static void srtla_source_show(void *data)
{
	struct srtla_source *context = data;
	if (context->media_source) obs_source_inc_showing(context->media_source);
}

static void srtla_source_hide(void *data)
{
	struct srtla_source *context = data;
	if (context->media_source) obs_source_dec_showing(context->media_source);
}

static bool protocol_changed(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	const char *proto = obs_data_get_string(settings, "protocol");
	bool is_rist = (proto && strcmp(proto, "rist") == 0);

	obs_property_set_visible(obs_properties_get(props, "rist_profile"), is_rist);
	obs_property_set_visible(obs_properties_get(props, "rist_buffer_ms"), is_rist);
	obs_property_set_visible(obs_properties_get(props, "rist_passphrase"), is_rist);
	obs_property_set_visible(obs_properties_get(props, "rist_key_size"), is_rist);
	obs_property_set_visible(obs_properties_get(props, "rist_stream_id"), is_rist);

	obs_property_set_description(obs_properties_get(props, "listen_ip"), 
		is_rist ? "RIST Bind IP (empty for ANY)" : "SRTLA Bind IP (empty for ANY)");
	obs_property_set_description(obs_properties_get(props, "listen_port"), 
		is_rist ? "RIST Listen Port (UDP)" : "SRTLA Listen Port (UDP)");
	
	return true;
}

static obs_properties_t *srtla_source_get_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();
	obs_properties_set_flags(props, OBS_PROPERTIES_DEFER_UPDATE);

	obs_property_t *proto_list = obs_properties_add_list(props, "protocol", "Protocol", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(proto_list, "SRTLA (Bonded SRT)", "srtla");
	obs_property_list_add_string(proto_list, "RIST (Reliable Internet Stream Transport)", "rist");
	obs_property_set_modified_callback(proto_list, protocol_changed);

	obs_property_t *engine_list = obs_properties_add_list(props, "playback_engine", "Playback Engine", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(engine_list, "FFmpeg (Built-in Media Source)", "ffmpeg");
	obs_property_list_add_string(engine_list, "VLC (VLC Video Source - Recommended)", "vlc");

	obs_properties_add_text(props, "listen_ip", "SRTLA Bind IP (empty for ANY)", OBS_TEXT_DEFAULT);
	obs_properties_add_int(props, "listen_port", "SRTLA Listen Port (UDP)", 1, 65535, 1);
	
	obs_property_t *prof_list = obs_properties_add_list(props, "rist_profile", "RIST Profile", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(prof_list, "Simple", 0);
	obs_property_list_add_int(prof_list, "Main", 1);
	obs_property_list_add_int(prof_list, "Advanced", 2);
	
	obs_properties_add_int(props, "rist_buffer_ms", "RIST Buffer / Latency (ms)", 0, 30000, 100);
	obs_properties_add_text(props, "rist_passphrase", "RIST Encryption Passphrase", OBS_TEXT_PASSWORD);
	
	obs_property_t *key_list = obs_properties_add_list(props, "rist_key_size", "RIST Key Size", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(key_list, "128", 128);
	obs_property_list_add_int(key_list, "256", 256);
	
	obs_properties_add_int(props, "rist_stream_id", "RIST Stream ID (0 for default)", 0, 65535, 1);

	obs_properties_add_int(props, "local_srt_port", "Local Internal Relay Port", 1, 65535, 1);

	return props;
}

static void srtla_source_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "protocol", "srtla");
	obs_data_set_default_string(settings, "playback_engine", "ffmpeg");
	obs_data_set_default_string(settings, "listen_ip", "");
	obs_data_set_default_int(settings, "listen_port", 5000);
	obs_data_set_default_int(settings, "local_srt_port", 4000);
	
	obs_data_set_default_int(settings, "rist_profile", 1);
	obs_data_set_default_int(settings, "rist_buffer_ms", 1000);
	obs_data_set_default_string(settings, "rist_passphrase", "");
	obs_data_set_default_int(settings, "rist_key_size", 128);
	obs_data_set_default_int(settings, "rist_stream_id", 0);
}

static void srtla_source_video_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);
	struct srtla_source *context = data;
	
	if (context) {
		uint64_t now = os_gettime_ns();
		if (context->needs_reload) {
			context->needs_reload = false;
			srtla_reload_media_source(context);
		}
	}
}

struct obs_source_info srtla_source_info = {
	.id = "srtla_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_CUSTOM_DRAW |
			OBS_SOURCE_DO_NOT_DUPLICATE,
	.get_name = srtla_source_get_name,
	.create = srtla_source_create,
	.destroy = srtla_source_destroy,
	.update = srtla_source_update,
	.get_properties = srtla_source_get_properties,
	.get_defaults = srtla_source_get_defaults,
	.activate = srtla_source_activate,
	.deactivate = srtla_source_deactivate,
	.show = srtla_source_show,
	.hide = srtla_source_hide,
	.video_render = srtla_source_video_render,
	.video_tick = srtla_source_video_tick,
	.get_width = srtla_source_get_width,
	.get_height = srtla_source_get_height,
};

void srtla_force_stop(void *data)
{
#ifdef _WIN32
	__try {
#endif
		struct srtla_source *context = data;
		if (context) {
			srtla_reset_group_by_port(context->listen_port);
			srtla_stop_thread(context);
			srtla_destroy_media_source(context);
			context->was_connected = false;
			context->connected_since = 0;
			context->last_audio_time = 0;
			context->starved_since = 0;
			context->last_recovery_time = 0;
			context->recovery_attempts = 0;
			context->pending_recovery = RECOVERY_NONE;
		}
#ifdef _WIN32
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		obs_log(LOG_ERROR, "[SRTLA] SEH Exception caught in srtla_force_stop! OBS crash prevented.");
	}
#endif
}

void srtla_force_start(void *data)
{
	struct srtla_source *context = data;
	obs_data_t *settings = obs_source_get_settings(context->source);
	srtla_source_update(context, settings);
	obs_data_release(settings);
}

void srtla_force_stop_all()
{
	struct srtla_source **targets = NULL;
	int count = 0;
	pthread_mutex_lock(&sources_mutex);
	for (struct srtla_source *s = sources_head; s; s = s->next)
		count++;
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

void srtla_force_start_all()
{
	struct srtla_source **targets = NULL;
	int count = 0;
	pthread_mutex_lock(&sources_mutex);
	for (struct srtla_source *s = sources_head; s; s = s->next)
		count++;
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

void srtla_force_restart_all()
{
	struct srtla_source **targets = NULL;
	int count = 0;
	pthread_mutex_lock(&sources_mutex);
	for (struct srtla_source *s = sources_head; s; s = s->next)
		count++;
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

void srtla_auto_recover_hung_sources()
{
	struct srtla_source **reload_targets = NULL;
	int reload_count = 0;
	
	struct srtla_source **restart_targets = NULL;
	int restart_count = 0;
	
	uint64_t now = os_gettime_ns();

	pthread_mutex_lock(&sources_mutex);
	for (struct srtla_source *s = sources_head; s; s = s->next) {
		if (!s->thread_running) continue;
		
		int groups = 0;
		if (s->protocol == PROTOCOL_RIST) {
			groups = rist_get_peer_count_by_port(s->listen_port);
		} else {
			groups = srtla_get_group_count_by_port(s->listen_port);
		}
		if (groups > 0) {
			if (s->connected_since == 0) {
				s->connected_since = now;
				s->last_recovery_time = now;
				s->recovery_attempts = 0;
			}

			// Cooldown of at least 10 seconds between recovery actions
			bool in_cooldown = (s->last_recovery_time > 0 && (now - s->last_recovery_time < 10000000000ULL));

			// Condition A: Video has been rendering for >= 12s, but zero audio packets ever arrived
			bool missing_on_connect = (s->last_audio_time == 0 && s->last_video_time > 0 &&
						   (now - s->connected_since >= 12000000000ULL) &&
						   (now - s->last_video_time < 2000000000ULL));

			// Condition B: Audio was receiving normally, but completely stopped for >= 8s while video is still active
			bool audio_dropped = (s->last_audio_time > 0 && (now - s->last_audio_time >= 8000000000ULL) &&
					      s->last_video_time > 0 && (now - s->last_video_time < 2000000000ULL));

			// Condition C: Audio severely starved (< 30kHz) continuously for >= 6s
			bool audio_starved = false;
			if (s->last_audio_time > 0 && (now - s->last_audio_time < 2000000000ULL) &&
			    s->current_stream_hz > 0 && s->current_stream_hz < 30000.0) {
				if (s->starved_since == 0) {
					s->starved_since = now;
				} else if (now - s->starved_since >= 6000000000ULL) {
					audio_starved = true;
				}
			} else {
				s->starved_since = 0;
			}

			// Condition D: Audio is receiving normally, but video has completely stopped for >= 8s
			bool video_dropped = (s->last_audio_time > 0 && (now - s->last_audio_time < 2000000000ULL) &&
					      s->last_video_time > 0 && (now - s->last_video_time >= 8000000000ULL));

			// Only attempt auto-recovery up to 2 times to avoid looping on video-only or mic-less streams
			if (!in_cooldown && s->recovery_attempts < 2 && (missing_on_connect || audio_dropped || audio_starved || video_dropped)) {
				s->recovery_attempts++;
				s->auto_reset_count++;
				s->last_recovery_time = now;
				s->starved_since = 0;

				if (missing_on_connect) {
					obs_log(LOG_WARNING,
						"[SRTLA] Port %d video active for >12s but no audio received! Auto-reloading media player (attempt %d/2).",
						s->listen_port, s->recovery_attempts);
				} else if (audio_dropped) {
					obs_log(LOG_WARNING,
						"[SRTLA] Port %d audio stopped for >8s while video active! Auto-reloading media player (attempt %d/2).",
						s->listen_port, s->recovery_attempts);
				} else if (video_dropped) {
					obs_log(LOG_WARNING,
						"[SRTLA] Port %d video stopped for >8s while audio active! Auto-reloading media player (attempt %d/2).",
						s->listen_port, s->recovery_attempts);
				} else {
					obs_log(LOG_WARNING,
						"[SRTLA] Port %d audio starved (%.1f Hz < 30kHz) for >6s! Auto-reloading media player (attempt %d/2).",
						s->listen_port, s->current_stream_hz, s->recovery_attempts);
				}
				s->pending_recovery = RECOVERY_RELOAD;
				reload_count++;
			}
		} else {
			// No active connections / groups: reset all connection & audio timers
			s->connected_since = 0;
			s->last_video_time = 0;
			s->last_audio_time = 0;
			s->starved_since = 0;
			s->current_stream_hz = 0;
			s->last_recovery_time = 0;
			s->recovery_attempts = 0;
			s->pending_recovery = RECOVERY_NONE;
		}
	}
	
	if (restart_count > 0) {
		restart_targets = calloc(restart_count, sizeof(struct srtla_source *));
		int i = 0;
		for (struct srtla_source *s = sources_head; s; s = s->next) {
			if (s->pending_recovery == RECOVERY_RESTART) {
				restart_targets[i++] = s;
				s->pending_recovery = RECOVERY_NONE;
			}
		}
	}

	if (reload_count > 0) {
		reload_targets = calloc(reload_count, sizeof(struct srtla_source *));
		int i = 0;
		for (struct srtla_source *s = sources_head; s; s = s->next) {
			if (s->pending_recovery == RECOVERY_RELOAD) {
				reload_targets[i++] = s;
				s->pending_recovery = RECOVERY_NONE;
			}
		}
	}
	pthread_mutex_unlock(&sources_mutex);

	for (int i = 0; i < restart_count; i++) {
		if (restart_targets[i]) {
			srtla_force_stop(restart_targets[i]);
			srtla_force_start(restart_targets[i]);
		}
	}
	if (restart_targets) free(restart_targets);

	for (int i = 0; i < reload_count; i++) {
		if (reload_targets[i]) {
			srtla_reload_media_source(reload_targets[i]);
		}
	}
	if (reload_targets) free(reload_targets);
}

void srtla_get_all_receivers_json(char *out_buffer, int max_len)
{
	int offset = 0;
	if (out_buffer && max_len > 0) {
		offset += snprintf(out_buffer + offset, max_len - offset, "[");
		bool first = true;
		pthread_mutex_lock(&sources_mutex);
		for (struct srtla_source *s = sources_head; s; s = s->next) {
			if (!first)
				offset += snprintf(out_buffer + offset, max_len - offset, ",");
			first = false;
			const char *name = obs_source_get_name(s->source);
			offset += snprintf(out_buffer + offset, max_len - offset,
					   "{\"name\":\"%s\",\"listen_port\":%d,\"running\":%s,\"protocol\":\"%s\",\"audio_drift_ms\":%lld,\"audio_db\":%.1f,\"auto_reset_count\":%d}",
					   name ? name : "Unknown", s->listen_port,
					   s->thread_running ? "true" : "false",
					   s->protocol == PROTOCOL_RIST ? "rist" : "srtla",
					   (long long)(s->current_audio_drift / 1000000LL),
					   s->current_audio_db,
					   s->auto_reset_count);
		}
		pthread_mutex_unlock(&sources_mutex);
		snprintf(out_buffer + offset, max_len - offset, "]");
	}
}

void srtla_force_start_by_name(const char *name)
{
	if (!name)
		return;
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

void srtla_force_stop_by_name(const char *name)
{
	if (!name)
		return;
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

void srtla_force_restart_by_name(const char *name)
{
	if (!name)
		return;
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

void srtla_force_reload_by_name(const char *name)
{
	if (!name)
		return;
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
		srtla_reload_media_source(target);
	}
}

void srtla_force_reload_all()
{
	struct srtla_source **targets = NULL;
	int count = 0;
	pthread_mutex_lock(&sources_mutex);
	for (struct srtla_source *s = sources_head; s; s = s->next)
		count++;
	if (count > 0) {
		targets = calloc(count, sizeof(struct srtla_source *));
		int i = 0;
		for (struct srtla_source *s = sources_head; s; s = s->next) {
			targets[i++] = s;
		}
	}
	pthread_mutex_unlock(&sources_mutex);

	for (int i = 0; i < count; i++) {
		if (targets[i]) {
			srtla_reload_media_source(targets[i]);
		}
	}
	free(targets);
}

void srtla_populate_receivers_list(obs_property_t *p) {
	obs_property_list_clear(p);
	pthread_mutex_lock(&sources_mutex);
	for (struct srtla_source *s = sources_head; s; s = s->next) {
		const char *name = obs_source_get_name(s->source);
		char port_str[32];
		snprintf(port_str, sizeof(port_str), "%d", s->listen_port);
		obs_property_list_add_string(p, name ? name : "Unknown", port_str);
	}
	pthread_mutex_unlock(&sources_mutex);
}
