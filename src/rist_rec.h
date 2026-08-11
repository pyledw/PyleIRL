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

int rist_rec_main(const struct rist_config *config);

#ifdef __cplusplus
}
#endif
