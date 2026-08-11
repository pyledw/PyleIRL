#include "rist_rec.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <util/bmem.h>
#include <obs-module.h>
#include <jansson.h>
#include <pthread.h>
#include <librist/librist.h>
#include <util/platform.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

// We need a thread sleep helper
#ifdef _WIN32
#define sleep_ms(x) Sleep(x)
#else
#define sleep_ms(x) usleep((x) * 1000)
#endif

struct rist_rec_context {
    struct rist_ctx *ctx;
    struct rist_peer *peer;
    int udp_socket;
    struct sockaddr_in dest_addr;
};

// Tracking structure for RIST receivers
#define MAX_RIST_INSTANCES 32

typedef struct {
    int peer_id;
    uint64_t cumulative_bytes;
    uint64_t last_seen_ms;
    char ip[128];
    int port;
} rist_peer_t;

typedef struct {
    int listen_port;
    bool is_listening;
    uint64_t total_bytes;
    int peer_count;
    char stats_json[4096]; // Cached JSON stats from librist
    rist_peer_t peers[32]; // Store active peers robustly
} rist_ctx_info_t;

static rist_ctx_info_t *global_rist_contexts[MAX_RIST_INSTANCES] = {0};
static pthread_mutex_t global_rist_mutex = PTHREAD_MUTEX_INITIALIZER;

static int add_rist_context(int listen_port) {
    pthread_mutex_lock(&global_rist_mutex);
    for (int i = 0; i < MAX_RIST_INSTANCES; i++) {
        if (!global_rist_contexts[i]) {
            global_rist_contexts[i] = bzalloc(sizeof(rist_ctx_info_t));
            global_rist_contexts[i]->listen_port = listen_port;
            global_rist_contexts[i]->is_listening = true;
            pthread_mutex_unlock(&global_rist_mutex);
            return i;
        }
    }
    pthread_mutex_unlock(&global_rist_mutex);
    return -1;
}

static void remove_rist_context(int index) {
    if (index >= 0 && index < MAX_RIST_INSTANCES) {
        pthread_mutex_lock(&global_rist_mutex);
        if (global_rist_contexts[index]) {
            bfree(global_rist_contexts[index]);
            global_rist_contexts[index] = NULL;
        }
        pthread_mutex_unlock(&global_rist_mutex);
    }
}

static int init_udp_relay(struct rist_rec_context *ctx, int local_port) {
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        blog(LOG_ERROR, "[RIST] WSAStartup failed");
        return -1;
    }
#endif

    ctx->udp_socket = (int)socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (ctx->udp_socket < 0) {
        blog(LOG_ERROR, "[RIST] Failed to create UDP socket for relay");
        return -1;
    }

    memset(&ctx->dest_addr, 0, sizeof(ctx->dest_addr));
    ctx->dest_addr.sin_family = AF_INET;
    ctx->dest_addr.sin_port = htons(local_port);
    inet_pton(AF_INET, "127.0.0.1", &ctx->dest_addr.sin_addr);

    return 0;
}

static void close_udp_relay(struct rist_rec_context *ctx) {
    if (ctx->udp_socket >= 0) {
#ifdef _WIN32
        closesocket(ctx->udp_socket);
        WSACleanup();
#else
        close(ctx->udp_socket);
#endif
        ctx->udp_socket = -1;
    }
}

static int rist_log_cb(void *arg, enum rist_log_level level, const char *msg) {
    (void)arg;
    int obs_level = LOG_INFO;
    if (level <= RIST_LOG_ERROR) obs_level = LOG_ERROR;
    else if (level == RIST_LOG_WARN) obs_level = LOG_WARNING;
    else if (level <= RIST_LOG_INFO) obs_level = LOG_INFO;
    else obs_level = LOG_DEBUG;

    // librist messages have newlines, strip them for OBS logs
    char buf[1024];
    strncpy(buf, msg, sizeof(buf)-1);
    buf[sizeof(buf)-1] = '\0';
    size_t len = strlen(buf);
    if (len > 0 && buf[len-1] == '\n') buf[len-1] = '\0';

    blog(obs_level, "[librist] %s", buf);
    return 0;
}

static int rist_stats_cb(void *arg, const struct rist_stats *stats_container) {
    int ctx_index = (int)(intptr_t)arg;
    if (ctx_index < 0 || ctx_index >= MAX_RIST_INSTANCES) return 0;
    
    if (stats_container->stats_type == RIST_STATS_RECEIVER_FLOW) {
        pthread_mutex_lock(&global_rist_mutex);
        rist_ctx_info_t *info = global_rist_contexts[ctx_index];
        if (info && stats_container->stats_json) {
            info->peer_count = stats_container->stats.receiver_flow.peer_count;
            info->total_bytes += stats_container->stats.receiver_flow.received;
            
            // Parse JSON to accumulate bytes per peer
            json_error_t err;
            json_t *root = json_loads(stats_container->stats_json, 0, &err);
            
            static uint64_t last_json_dump_ms = 0;
            uint64_t current_ms = os_gettime_ns() / 1000000;
            if (current_ms - last_json_dump_ms > 10000) {
                blog(LOG_INFO, "[RIST DEBUG JSON] %s", stats_container->stats_json);
                last_json_dump_ms = current_ms;
            }
            
            if (root) {
                json_t *recv_stats = json_object_get(root, "receiver-stats");
                json_t *flow_inst = json_object_get(recv_stats, "flowinstant");
                json_t *peers = json_object_get(flow_inst, "peers");
                
                uint64_t now_ms = os_gettime_ns() / 1000000;
                
                if (peers && json_is_array(peers)) {
                    size_t index;
                    json_t *value;
                    json_array_foreach(peers, index, value) {
                        json_t *peer_stats = json_object_get(value, "stats");
                        if (peer_stats) {
                            int peer_id = (int)json_integer_value(json_object_get(value, "id"));
                            json_t *bitrate_obj = json_object_get(peer_stats, "bitrate");
                            double bitrate = json_is_real(bitrate_obj) ? json_real_value(bitrate_obj) : json_integer_value(bitrate_obj);
                            
                            json_t *url_obj = json_object_get(value, "url");
                            const char *url_str = url_obj ? json_string_value(url_obj) : NULL;
                            char ip_str[128] = {0};
                            int parsed_port = 0;
                            if (url_str && strncmp(url_str, "rist://", 7) == 0) {
                                // Find the last colon for port
                                const char *colon = strrchr(url_str, ':');
                                if (colon && colon > url_str + 6) {
                                    parsed_port = atoi(colon + 1);
                                    int ip_len = (int)(colon - (url_str + 7));
                                    if (ip_len > 0 && ip_len < 127) {
                                        strncpy(ip_str, url_str + 7, ip_len);
                                    }
                                } else {
                                    strncpy(ip_str, url_str + 7, 127);
                                }
                            }
                            if (ip_str[0] == '\0') {
                                snprintf(ip_str, sizeof(ip_str), "rist-peer-%d", peer_id);
                            }
                            
                            // Find or create peer
                            int slot = -1;
                            for (int i = 0; i < 32; i++) {
                                if (info->peers[i].peer_id == peer_id && info->peers[i].last_seen_ms > 0) {
                                    slot = i;
                                    break;
                                } else if (slot == -1 && info->peers[i].last_seen_ms == 0) {
                                    slot = i;
                                }
                            }
                            
                            if (slot >= 0) {
                                info->peers[slot].peer_id = peer_id;
                                strncpy(info->peers[slot].ip, ip_str, 127);
                                info->peers[slot].port = parsed_port;
                                // 500ms interval, so add (bitrate / 8) / 2 bytes to cumulative total
                                info->peers[slot].cumulative_bytes += (uint64_t)(bitrate / 16.0);
                                info->peers[slot].last_seen_ms = now_ms;
                            }
                        }
                    }
                }
                
                // Cleanup stale peers (not seen in 2 seconds)
                for (int i = 0; i < 32; i++) {
                    if (info->peers[i].last_seen_ms > 0 && (now_ms - info->peers[i].last_seen_ms > 2000)) {
                        info->peers[i].last_seen_ms = 0;
                        info->peers[i].cumulative_bytes = 0;
                    }
                }
                
                json_decref(root);
            }
        }
        pthread_mutex_unlock(&global_rist_mutex);
    }
    
    return 0; // return 0 allows librist to free the stats_container itself
}

int rist_rec_main(const struct rist_config *config) {
    struct rist_rec_context relay = {0};
    relay.udp_socket = -1;
    struct rist_logging_settings *logging_settings = NULL;
    int ret = 0;

    blog(LOG_INFO, "[RIST] Starting RIST receiver on %s:%d, relaying to 127.0.0.1:%d", 
            config->listen_ip[0] ? config->listen_ip : "0.0.0.0", 
            config->listen_port, config->local_srt_port);

    if (init_udp_relay(&relay, config->local_srt_port) != 0) {
        return -1;
    }

    // Determine RIST profile
    enum rist_profile profile = RIST_PROFILE_MAIN;
    if (config->profile == 0) profile = RIST_PROFILE_SIMPLE;
    else if (config->profile == 1) profile = RIST_PROFILE_MAIN;
    else if (config->profile == 2) profile = RIST_PROFILE_ADVANCED;

    if (rist_logging_set(&logging_settings, RIST_LOG_DEBUG, rist_log_cb, NULL, NULL, NULL) != 0) {
        blog(LOG_ERROR, "[RIST] Failed to setup librist logging");
        close_udp_relay(&relay);
        return -1;
    }

    if (rist_receiver_create(&relay.ctx, profile, logging_settings) != 0) {
        blog(LOG_ERROR, "[RIST] Could not create RIST receiver context");
        rist_logging_settings_free2(&logging_settings);
        close_udp_relay(&relay);
        return -1;
    }

    // Set buffer
    uint32_t recovery_ms = config->buffer_ms > 0 ? config->buffer_ms : 1000;
    
    // Create peer
    struct rist_peer_config *peer_config = NULL;

    char url[256];
    snprintf(url, sizeof(url), "rist://@%s:%d?session-timeout=5000", config->listen_ip[0] ? config->listen_ip : "0.0.0.0", config->listen_port);

    // Parse URL
    if (rist_parse_address2(url, &peer_config) != 0) {
        blog(LOG_ERROR, "[RIST] Failed to parse URL: %s", url);
        free((void *)peer_config);
        rist_destroy(relay.ctx);
        rist_logging_settings_free2(&logging_settings);
        close_udp_relay(&relay);
        return -1;
    }

    peer_config->recovery_length_min = recovery_ms;
    peer_config->recovery_length_max = recovery_ms;
    peer_config->recovery_mode = RIST_RECOVERY_MODE_TIME;
    
    // Apply encryption if set
    if (config->passphrase[0] != '\0') {
        strncpy((char *)peer_config->secret, config->passphrase, 127);
        peer_config->key_size = config->key_size == 256 ? 256 : 128;
    }

    if (rist_peer_create(relay.ctx, &relay.peer, peer_config) != 0) {
        blog(LOG_ERROR, "[RIST] Failed to create RIST peer");
        free((void *)peer_config);
        rist_destroy(relay.ctx);
        rist_logging_settings_free2(&logging_settings);
        close_udp_relay(&relay);
        return -1;
    }
    free((void *)peer_config);

    if (rist_start(relay.ctx) != 0) {
        blog(LOG_ERROR, "[RIST] Failed to start RIST receiver");
        rist_destroy(relay.ctx);
        rist_logging_settings_free2(&logging_settings);
        close_udp_relay(&relay);
        return -1;
    }

    int ctx_index = add_rist_context(config->listen_port);
    if (ctx_index >= 0) {
        rist_stats_callback_set(relay.ctx, 500, rist_stats_cb, (void *)(intptr_t)ctx_index);
    }

    blog(LOG_INFO, "[RIST] RIST Receiver started successfully on port %d", config->listen_port);

    // Main read loop
    while (config->stop_flag && !*config->stop_flag) {
        const struct rist_data_block *data_block = NULL;
        int queue_size = rist_receiver_data_read(relay.ctx, &data_block, 50); // 50ms timeout

        if (queue_size > 0 && data_block && data_block->payload) {
            // Send payload to local UDP socket
            sendto(relay.udp_socket, (const char *)data_block->payload, (int)data_block->payload_len, 0,
                   (struct sockaddr *)&relay.dest_addr, sizeof(relay.dest_addr));
            
            rist_receiver_data_block_free2(&data_block);
        } else if (queue_size < 0) {
            blog(LOG_ERROR, "[RIST] Data read error, queue_size=%d", queue_size);
            // Optionally sleep or break
            sleep_ms(10);
        }
    }

    blog(LOG_INFO, "[RIST] Stopping RIST receiver");
    
    if (ctx_index >= 0) {
        remove_rist_context(ctx_index);
    }
    
    rist_destroy(relay.ctx);
    rist_logging_settings_free2(&logging_settings);
    close_udp_relay(&relay);

    return 0;
}

void rist_get_connection_stats(bool *is_listening, int *active_groups, int *active_connections) {
    pthread_mutex_lock(&global_rist_mutex);
    for (int i = 0; i < MAX_RIST_INSTANCES; i++) {
        rist_ctx_info_t *info = global_rist_contexts[i];
        if (info && info->is_listening) {
            if (is_listening) *is_listening = true;
            if (active_groups) (*active_groups)++;
            if (active_connections) (*active_connections) += info->peer_count;
        }
    }
    pthread_mutex_unlock(&global_rist_mutex);
}

int rist_get_peer_count_by_port(int listen_port) {
    int count = 0;
    pthread_mutex_lock(&global_rist_mutex);
    for (int i = 0; i < MAX_RIST_INSTANCES; i++) {
        rist_ctx_info_t *info = global_rist_contexts[i];
        if (info && info->listen_port == listen_port && info->is_listening) {
            count = info->peer_count;
            break;
        }
    }
    pthread_mutex_unlock(&global_rist_mutex);
    return count;
}

void rist_get_connection_details(char* out_buffer, int max_len) {
    if (!out_buffer || max_len <= 0) return;
    
    int offset = 0;
    offset += snprintf(out_buffer + offset, max_len - offset, "{");
    
    pthread_mutex_lock(&global_rist_mutex);
    
    // Add active ports array
    offset += snprintf(out_buffer + offset, max_len - offset, "\"ports\":[");
    bool first_port = true;
    for (int i = 0; i < MAX_RIST_INSTANCES; i++) {
        rist_ctx_info_t *info = global_rist_contexts[i];
        if (info && info->is_listening) {
            if (!first_port) offset += snprintf(out_buffer + offset, max_len - offset, ",");
            first_port = false;
            offset += snprintf(out_buffer + offset, max_len - offset, "%d", info->listen_port);
        }
    }
    offset += snprintf(out_buffer + offset, max_len - offset, "],");
    
    // Add groups array
    offset += snprintf(out_buffer + offset, max_len - offset, "\"groups\":[");
    bool first_group = true;
    for (int i = 0; i < MAX_RIST_INSTANCES; i++) {
        rist_ctx_info_t *info = global_rist_contexts[i];
        if (info && info->is_listening) {
            if (!first_group) offset += snprintf(out_buffer + offset, max_len - offset, ",");
            first_group = false;
            
            // Re-format RIST JSON into SRTLA JSON format
            // Group ID for RIST can just be the listen port + 10000 to keep it unique
            offset += snprintf(out_buffer + offset, max_len - offset, "{\"id\":%llu,\"bytes\":%llu,\"listen_port\":%d,\"conns\":[", 
                (unsigned long long)(info->listen_port + 10000), (unsigned long long)info->total_bytes, info->listen_port);
            
            bool first_conn = true;
            uint64_t now_ms = os_gettime_ns() / 1000000;
            for (int p = 0; p < 32; p++) {
                if (info->peers[p].last_seen_ms > 0 && (now_ms - info->peers[p].last_seen_ms <= 2000)) {
                    if (!first_conn) offset += snprintf(out_buffer + offset, max_len - offset, ",");
                    first_conn = false;
                    
                    offset += snprintf(out_buffer + offset, max_len - offset, "{\"ip\":\"%s\",\"port\":%d,\"bytes\":%llu}", 
                        info->peers[p].ip, info->peers[p].port, (unsigned long long)info->peers[p].cumulative_bytes);
                }
            }
            offset += snprintf(out_buffer + offset, max_len - offset, "]}");
        }
    }
    pthread_mutex_unlock(&global_rist_mutex);
    snprintf(out_buffer + offset, max_len - offset, "]}");
}
