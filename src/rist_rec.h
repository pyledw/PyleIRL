#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Structure to pass configuration for the RIST receiver
struct rist_config {
    char listen_ip[64];
    int listen_port;
    int local_srt_port;
    volatile int *stop_flag;
    
    int profile;           // e.g. RIST_PROFILE_MAIN
    int buffer_ms;         // Latency/ARQ buffer size
    char passphrase[128];  // Encryption passphrase
    int key_size;          // 128 or 256
    int stream_id;         // Optional stream ID
};

// Receiver API
int rist_rec_main(const struct rist_config *config);

// Stats API
void rist_get_connection_stats(bool *is_listening, int *active_groups, int *active_connections);
void rist_get_connection_details(char* out_buffer, int max_len);
int rist_get_peer_count_by_port(int listen_port);

#ifdef __cplusplus
}
#endif
