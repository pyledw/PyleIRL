#include "rist_rec.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <obs-module.h>
#include <librist/librist.h>

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

int rist_rec_main(const struct rist_config *config) {
    struct rist_rec_context relay = {0};
    relay.udp_socket = -1;
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

    if (rist_receiver_create(&relay.ctx, profile, NULL) != 0) {
        blog(LOG_ERROR, "[RIST] Could not create RIST receiver context");
        close_udp_relay(&relay);
        return -1;
    }

    // Set buffer
    uint32_t recovery_ms = config->buffer_ms > 0 ? config->buffer_ms : 1000;
    
    // Create peer
    struct rist_peer_config *peer_config = NULL;

    char url[256];
    snprintf(url, sizeof(url), "rist://@%s:%d", config->listen_ip[0] ? config->listen_ip : "0.0.0.0", config->listen_port);

    // Parse URL
    if (rist_parse_address2(url, &peer_config) != 0) {
        blog(LOG_ERROR, "[RIST] Failed to parse URL: %s", url);
        free((void *)peer_config);
        rist_destroy(relay.ctx);
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
        close_udp_relay(&relay);
        return -1;
    }
    free((void *)peer_config);

    if (rist_start(relay.ctx) != 0) {
        blog(LOG_ERROR, "[RIST] Failed to start RIST receiver");
        rist_destroy(relay.ctx);
        close_udp_relay(&relay);
        return -1;
    }

    blog(LOG_INFO, "[RIST] RIST Receiver started successfully");

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
    rist_destroy(relay.ctx);
    close_udp_relay(&relay);

    return 0;
}
