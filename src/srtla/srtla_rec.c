/*
    srtla - SRT transport proxy with link aggregation
    Copyright (C) 2020-2021 BELABOX project

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h> 
#else
#include <io.h>
#endif
#include <time.h> 
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <stdio.h>
#define htobe32(x) htonl(x)
#define be32toh(x) ntohl(x)
typedef unsigned long in_addr_t; // Define in_addr_t for Windows

// Update sendto, recv, and recvfrom calls for Winsock compatibility
static inline int get_addr_len(const struct sockaddr *addr) {
  if (addr == NULL) return 128; // sizeof(struct sockaddr_storage)
  if (addr->sa_family == AF_INET) return 16; // sizeof(struct sockaddr_in)
  if (addr->sa_family == AF_INET6) return 28; // sizeof(struct sockaddr_in6)
  return 128;
}

// Update sendto, recv, and recvfrom calls for Winsock compatibility
#define SENDTO(sock, buf, len, flags, addr, addrlen) sendto(sock, (const char *)(buf), len, flags, (const struct sockaddr *)(addr), get_addr_len((const struct sockaddr *)(addr)))
#define RECV(sock, buf, len, flags) recv(sock, (char *)(buf), len, flags)
#define RECVFROM(sock, buf, len, flags, addr, addrlen) recvfrom(sock, (char *)(buf), len, flags, addr, addrlen)
#else
#include <sys/socket.h>
#include <endian.h>
#include <netinet/in.h>
#include <arpa/inet.h>
static inline int get_addr_len(const struct sockaddr *addr) {
  if (addr == NULL) return 128;
  if (addr->sa_family == AF_INET) return 16;
  if (addr->sa_family == AF_INET6) return 28;
  return 128;
}
#define SENDTO(sock, buf, len, flags, addr, addrlen) sendto(sock, buf, len, flags, (const struct sockaddr *)(addr), get_addr_len((const struct sockaddr *)(addr)))
#define RECV(sock, buf, len, flags) recv(sock, buf, len, flags)
#define RECVFROM(sock, buf, len, flags, addr, addrlen) recvfrom(sock, buf, len, flags, addr, addrlen)
#endif
#ifndef _WIN32
#include <netdb.h>
#endif
#include <sys/types.h>
#ifdef __linux__
#include <sys/epoll.h>
#endif
#include <errno.h>

#include "common.h"

#define MAX_CONNS_PER_GROUP 8
#define MAX_GROUPS          200

#define CLEANUP_PERIOD 3
#define GROUP_TIMEOUT  10
#define CONN_TIMEOUT   10

#define RECV_ACK_INT 10
typedef struct srtla_conn {
  struct srtla_conn *next;
  struct sockaddr_storage addr;
  time_t last_rcvd;
  int recv_idx;
  uint32_t recv_log[RECV_ACK_INT];
  /* registration / reconnect state */
  int reg_attempts;
  time_t next_reg_try_ms;
  int backoff_ms;
  int had_fatal_error;
  uint64_t bytes_received;
} conn_t;

typedef struct srtla_conn_group {
  struct srtla_conn_group *next;
  conn_t *conns;
  time_t created_at;
  int srt_sock;
  struct sockaddr_storage last_addr;
  char id[SRTLA_ID_LEN];
  /* reconnection state */
  uint64_t logical_group_id;
  group_state state;
  time_t next_srt_retry_ms;
  int srt_retry_attempts;
  uint64_t bytes_received;
} conn_group_t;

typedef struct {

/*
Manual testing scenarios:

1) Start receiver + SRT listener + sender. Verify streaming flows normally.
2) Stop SRT listener (e.g., OBS). Verify receiver logs entering WAITING_SRT and schedules retries.
3) Restart SRT listener. Verify receiver transitions back to ACTIVE automatically.
4) On sender, disable a network interface and confirm only that connection reconnects while others remain.
5) Force ICMP Port Unreachable on Windows (close receiver port briefly) and confirm sockets are recreated and registration retried.
6) Repeatedly bring down/up SRT listener and confirm no unbounded memory growth (use counters/logs).

*/
  uint32_t type;
  uint32_t acks[RECV_ACK_INT];
} srtla_ack_pkt;

#define MAX_SRTLA_INSTANCES 32
#include <pthread.h>

typedef struct srtla_ctx {
    int _srtla_sock;
    struct sockaddr_storage _srt_addr;
    conn_group_t *_groups;
    int _group_count;
    long _active_connections;
    long _failed_connections;
    int _listen_port;
    uint64_t _group_seq;
    bool _is_listening;
} srtla_ctx_t;

#ifdef _WIN32
__declspec(thread) srtla_ctx_t *current_ctx = NULL;
#else
__thread srtla_ctx_t *current_ctx = NULL;
#endif

srtla_ctx_t *global_contexts[MAX_SRTLA_INSTANCES] = {0};
pthread_mutex_t global_ctx_mutex = PTHREAD_MUTEX_INITIALIZER;

#define srtla_sock current_ctx->_srtla_sock
#define srt_addr current_ctx->_srt_addr
#define groups current_ctx->_groups
#define group_count current_ctx->_group_count
#define srtla_active_connections current_ctx->_active_connections
#define srtla_failed_connections current_ctx->_failed_connections
#define srtla_listen_port current_ctx->_listen_port
#define global_group_seq current_ctx->_group_seq

#define ADDR_LEN sizeof(struct sockaddr_storage)

/* runtime flags */
int flag_auto_reconnect = 1;
int flag_log_errors = 0;
int flag_reconnect_interval_ms = 500;

FILE *urandom;

/*

Async I/O support

*/
#ifdef __linux__
int socket_epoll;

int epoll_add(int fd, uint32_t events, void *userdata) {
  struct epoll_event ev={0};
  ev.events = events;
  ev.data.ptr = userdata;
  return epoll_ctl(socket_epoll, EPOLL_CTL_ADD, fd, &ev);
}

int epoll_rem(int fd) {
  struct epoll_event ev; // non-NULL for Linux < 2.6.9, however unlikely it is
  return epoll_ctl(socket_epoll, EPOLL_CTL_DEL, fd, &ev);
}
#endif

/*

Misc helper functions

*/
void print_help() {
  fprintf(stderr,
          "Syntax: srtla_rec [-v] SRTLA_LISTEN_PORT SRT_HOST SRT_PORT\n\n"
          "-v      Print the version and exit\n");
}

int const_time_cmp(const void *a, const void *b, int len) {
  char diff = 0;
  char *ca = (char *)a;
  char *cb = (char *)b;
  for (int i = 0; i < len; i++) {
    diff |= *ca - *cb;
    ca++;
    cb++;
  }

  return diff ? -1 : 0;
}

int get_random(void *dest, size_t len) {
#ifdef _WIN32
  // Windows'ta CryptGenRandom kullanarak rastgele sayı üret
  HCRYPTPROV hCryptProv;
  if (!CryptAcquireContext(&hCryptProv, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
    fprintf(stderr, "CryptAcquireContext failed: %lu\n", GetLastError());
    return -1;
  }
  
  BOOL result = CryptGenRandom(hCryptProv, (DWORD)len, (BYTE*)dest);
  CryptReleaseContext(hCryptProv, 0);
  
  return result ? 0 : -1;
#else
  while (len) {
    int ret = fread(dest, 1, len, urandom);
    if (ret <= 0) return -1;
    len -= ret;
  }
  return 0;
#endif
}


/*

Connection and group management functions

*/
conn_group_t *group_find_by_id(char *id) {
  for (conn_group_t* g = groups; g != NULL; g = g->next) {
    if (const_time_cmp(g->id, id, SRTLA_ID_LEN) == 0) {
      return g;
    }
  }

  return NULL;
}

int group_find_by_addr(struct sockaddr *addr, conn_group_t **rg, conn_t **rc) {
  for (conn_group_t* g = groups; g != NULL; g = g->next) {
    for (conn_t *c = g->conns; c != NULL; c = c->next) {
      if (const_time_cmp((struct sockaddr*)&(c->addr), addr, ADDR_LEN) == 0) {
        *rg = g;
        *rc = c;
        return 1;
      }
    }
    if (const_time_cmp((struct sockaddr*)&g->last_addr, addr, ADDR_LEN) == 0) {
      *rg = g;
      *rc = NULL;
      return 0;
    }
  }

  return -1;
}

conn_group_t *group_create(char *sender_id, time_t ts) {
  // Make sure the ID isn't a duplicate - very unlikely
  char id[SRTLA_ID_LEN];
  memcpy(&id, sender_id, SRTLA_ID_LEN/2);
  do {
    int ret = get_random(&id[SRTLA_ID_LEN/2], SRTLA_ID_LEN/2);
    if (ret != 0) return NULL;
  } while(group_find_by_id(id) != NULL);

  // Allocate the new group
  conn_group_t *g = malloc(sizeof(conn_group_t));
  if (g == NULL) {
    err("malloc() failed\n");
    return NULL;
  }

  // And initialize it with the ID we've built above
  memcpy(&g->id, id, SRTLA_ID_LEN);
  g->conns = NULL;
  g->srt_sock = -1;
  g->logical_group_id = global_group_seq++;
  g->state = G_ACTIVE;
  g->next_srt_retry_ms = 0;
  g->srt_retry_attempts = 0;
  g->created_at = ts;
  g->next = groups;
  groups = g;

  return g;
}

conn_group_t *group_create_full(char *id, time_t ts) {
  // Make sure it doesn't exist just in case
  if (group_find_by_id(id) != NULL) return NULL;

  // Allocate the new group
  conn_group_t *g = malloc(sizeof(conn_group_t));
  if (g == NULL) {
    err("malloc() failed\n");
    return NULL;
  }

  // Initialize with the exact ID provided by the client
  memcpy(&g->id, id, SRTLA_ID_LEN);
  g->conns = NULL;
  g->srt_sock = -1;
  g->logical_group_id = global_group_seq++;
  g->state = G_ACTIVE;
  g->next_srt_retry_ms = 0;
  g->srt_retry_attempts = 0;
  g->created_at = ts;
  g->next = groups;
  groups = g;
  
  return g;
}

int group_destroy(conn_group_t *g, conn_group_t **prev_link) {
  if (g == NULL) return -1;

  for (conn_t *c = g->conns; c != NULL;) {
    conn_t *next = c->next;
#ifdef _WIN32
    InterlockedDecrement(&srtla_active_connections);
#endif
    free(c);
    c = next;
  }

  if (g->srt_sock > 0) {
#ifdef __linux__
    epoll_rem(g->srt_sock);
#endif
    close_socket(g->srt_sock);
  }

  if (prev_link != NULL) {
    // The caller passed us a pointer to the linked list pointer to this group
    *prev_link = g->next;
  } else {
    // Search and unlink
    for (conn_group_t **it = &groups; (*it) != NULL; it = &((*it)->next)) {
      if (*it == g) {
        *it = g->next;
        break;
      }
    } // for
  } // prev_link == NULL

  free(g);

  /* Must ensure statements updating group_count on the creation and
     destruction code paths match up so we don't drift */
  group_count--;

  return 0;
}

int group_count_conns(conn_group_t *g) {
  int count = 0;
  for (conn_t *c = g->conns; c != NULL; c = c->next) {
    count++;
  }
  return count;
}

int group_reg(struct sockaddr *addr, char *in_buf, time_t ts) {
  if (group_count >= MAX_GROUPS) {
    err("%s:%d: group count is %d, rejecting group registration\n",
        print_addr(addr), port_no(addr), group_count);
    goto err;
  }

  // If this remote address is already registered, remove it from the old group
  conn_group_t *g = NULL;
  conn_t *c = NULL;
  int ret = group_find_by_addr(addr, &g, &c);
  if (ret != -1 && c != NULL) {
    info("%s:%d was in old group %p, removing to allow new group registration\n", print_addr(addr), port_no(addr), g);
    for (conn_t **it = &g->conns; *it != NULL; it = &((*it)->next)) {
      if (*it == c) {
        *it = c->next;
        free(c);
        break;
      }
    }
  }

  // Allocate the group
  char *id = in_buf + 2;
  g = group_create(id, ts);
  if (g == NULL) goto err;

  /* Record the address used to register the group
     It won't be allowed to register another group while this one is active */
  memcpy(&g->last_addr, addr, sizeof(struct sockaddr_storage));

  // Build a REG2 packet
  char out_buf[SRTLA_TYPE_REG2_LEN];
  uint16_t header = htobe16(SRTLA_TYPE_REG2);
  memcpy(out_buf, &header, sizeof(header));
  memcpy(out_buf + sizeof(header), g->id, SRTLA_ID_LEN);

  // Send the REG2 packet
  ret = SENDTO(srtla_sock, out_buf, sizeof(out_buf), 0, (struct sockaddr*)addr, ADDR_LEN);
  if (ret != sizeof(out_buf)) {
    err("Failed to send REG2 packet to %s:%d (sent %d, expected %d, err=%s)\n",
        print_addr(addr), port_no(addr), ret, (int)sizeof(out_buf), sock_err_str());
    goto err_destroy;
  }

  info("%s:%d: group #%llu registered\n", print_addr(addr), port_no(addr), (unsigned long long)g->logical_group_id);

  // Only count the group after everything else succeeded
  group_count++;

  return 0;

err_destroy:
  groups = g->next;
  free(g);

err:
  err("%s:%d: group registration failed\n", print_addr(addr), port_no(addr));
  header = htobe16(SRTLA_TYPE_REG_ERR);
  SENDTO(srtla_sock, &header, sizeof(header), 0, (struct sockaddr*)addr, ADDR_LEN);
  return -1;
}

int conn_reg(struct sockaddr *addr, char *in_buf, time_t ts) {
  conn_group_t *g, *tmp;
  conn_t *c;

  char *id = in_buf + 2;
  g = group_find_by_id(id);
  if (g == NULL) {
    if (group_count >= MAX_GROUPS) {
      err("%s:%d: group count is %d, rejecting recovered group registration\n",
          print_addr(addr), port_no(addr), group_count);
      uint16_t header = htobe16(SRTLA_TYPE_REG_NGP);
      SENDTO(srtla_sock, &header, sizeof(header), 0, addr, sizeof(struct sockaddr_storage));
      goto err_early;
    }
    
    // Group not found, but this is a REG2 packet which contains the full ID!
    // The client is trying to reconnect an existing session that we lost (e.g. because we restarted).
    // Instead of sending NGP (which the client ignores due to a bug), let's seamlessly recover the session!
    g = group_create_full(id, ts);
    if (g == NULL) {
      uint16_t header = htobe16(SRTLA_TYPE_REG_NGP);
      int sent = SENDTO(srtla_sock, &header, sizeof(header), 0, addr, sizeof(struct sockaddr_storage));
      if (sent != sizeof(header)) {
        err("Failed to send REG_NGP to %s:%d (err=%s)\n", print_addr(addr), port_no(addr), sock_err_str());
      }
      goto err_early;
    }
    
    group_count++;
    info("%s:%d: seamlessly recovered lost group #%llu from REG2!\n", print_addr(addr), port_no(addr), (unsigned long long)g->logical_group_id);
  }

  // Check if we should register the connection
  tmp = NULL;
  int ret = group_find_by_addr(addr, &tmp, &c);
  if (ret == 1) { // Same IP + Port combination
    if (g != tmp) { // We were already registered to a different group
      info("%s:%d belongs to old group %p, moving it to new group %p\n",
          print_addr(addr), port_no(addr), tmp, g);
      
      // Remove from the old group's list
      for (conn_t **it = &tmp->conns; *it != NULL; it = &((*it)->next)) {
        if (*it == c) {
          *it = c->next;
          break;
        }
      }
      
      // We reuse the existing conn_t struct 'c'
      memset(c, 0, sizeof(*c));
      c->addr = *(struct sockaddr_storage *)addr;
      c->last_rcvd = ts;
      c->bytes_received = 0;
      c->next = g->conns;
      g->conns = c;
    } else {
      // It's already in the correct group! Just update it.
      c->last_rcvd = ts;
    }
  } else {
    // New connection
    c = malloc(sizeof(conn_t));
    if (c == NULL) {
      err("malloc() failed\n");
      goto err;
    }
    memset(c, 0, sizeof(*c));
    c->addr = *(struct sockaddr_storage *)addr;
    c->last_rcvd = ts;
    c->bytes_received = 0;
    c->next = g->conns;
    g->conns = c;
#ifdef _WIN32
    InterlockedIncrement(&srtla_active_connections);
#endif
  }


  uint16_t header = htobe16(SRTLA_TYPE_REG3);
  ret = SENDTO(srtla_sock, &header, sizeof(header), 0, addr, sizeof(struct sockaddr_storage));
  if (ret != sizeof(header)) {
    err("Failed to send REG3 packet to %s:%d (sent %d, expected %d, err=%s)\n",
        print_addr(addr), port_no(addr), ret, (int)sizeof(header), sock_err_str());
    goto err_destroy;
  }

  info("%s:%d (group %p): connection registration\n", print_addr(addr), port_no(addr), g);

  // If it all worked, mark this peer as the most recently active one
  memcpy(&g->last_addr, addr, sizeof(struct sockaddr_storage));

  return 0;

err_destroy:
  g->conns = c->next;
#ifdef _WIN32
  InterlockedDecrement(&srtla_active_connections);
#endif
  free(c);

err:
  header = htobe16(SRTLA_TYPE_REG_ERR);
  SENDTO(srtla_sock, &header, sizeof(header), 0, addr, sizeof(struct sockaddr_storage));

err_early:
  err("%s:%d: connection registration for group %p failed\n",
      print_addr(addr), port_no(addr), g);
  return -1;
}

/*

The main network event handlers

Resource limits:
  * connections per group MAX_CONNS_PER_GROUP
  * total groups          MAX_GROUPS

*/

void handle_srt_data(conn_group_t *g) {
  char buf[MTU];

  if (g == NULL) return;

  int n = RECV(g->srt_sock, &buf, MTU, 0);
  if (n < SRT_MIN_LEN) {
    int e = errno;
    if (flag_log_errors) err("Group #%llu (ptr=%p): SRT read failed (err=%s). Entering WAITING_SRT\n", (unsigned long long)g->logical_group_id, g, sock_err_str());
    else err("Group %p: failed to read the SRT sock, entering WAITING_SRT\n", g);
    // Close socket and mark for retry rather than destroying the whole group
    if (g->srt_sock > 0) { close_socket(g->srt_sock); }
    g->srt_sock = -1;
    if (flag_auto_reconnect) {
      g->state = G_WAITING_SRT;
      g->srt_retry_attempts++;
      g->next_srt_retry_ms = time(NULL) * 1000 + flag_reconnect_interval_ms * (1 << (g->srt_retry_attempts - 1));
      if (g->next_srt_retry_ms - (time(NULL)*1000) > REG_RETRY_MAX_MS) {
        g->next_srt_retry_ms = time(NULL)*1000 + REG_RETRY_MAX_MS;
      }
    } else {
      group_destroy(g, NULL);
    }
    return;
  }

  // ACK
  if (is_srt_ack(buf, n)) {
    // Broadcast SRT ACKs over all connections for timely delivery
    for (conn_t *c = g->conns; c != NULL; c = c->next) {
      int ret = SENDTO(srtla_sock, &buf, n, 0, (struct sockaddr*)&c->addr, sizeof(struct sockaddr_storage));
      if (ret != n) {
        if (flag_log_errors) err("%s:%d (group #%llu): failed to send the SRT ack (ret=%d, err=%s)\n",
                                 print_addr((struct sockaddr*)&c->addr), port_no((struct sockaddr*)&c->addr), (unsigned long long)g->logical_group_id, ret, sock_err_str());
        else err("%s:%d (group %p): failed to send the SRT ack\n",
                 print_addr((struct sockaddr*)&c->addr), port_no((struct sockaddr*)&c->addr), g);
      }
    }
  } else {
    // send other packets over the most recently used SRTLA connection
    int ret = SENDTO(srtla_sock, &buf, n, 0, (struct sockaddr*)&g->last_addr, sizeof(struct sockaddr_storage));
    if (ret != n) {
      int serr = errno;
      if (flag_log_errors) err("%s:%d (group #%llu): failed to send the SRT packet (ret=%d, err=%s)\n",
          print_addr((struct sockaddr*)&g->last_addr), port_no((struct sockaddr*)&g->last_addr), (unsigned long long)g->logical_group_id, ret, sock_err_str());
      else err("%s:%d (group %p): failed to send the SRT packet\n",
          print_addr((struct sockaddr*)&g->last_addr), port_no((struct sockaddr*)&g->last_addr), g);
      // If fatal, mark connection for reconnect
      if (is_fatal_udp_error(serr)) {
        // remove the connection immediately
        for (conn_t **it = &g->conns; *it != NULL; it = &((*it)->next)) {
          if (const_time_cmp((struct sockaddr*)&((*it)->addr), (struct sockaddr*)&g->last_addr, sizeof(struct sockaddr_storage)) == 0) {
            conn_t *dead = *it;
            *it = dead->next;
#ifdef _WIN32
            InterlockedDecrement(&srtla_active_connections);
            InterlockedIncrement(&srtla_failed_connections);
#endif
            free(dead);
            break;
          }
        }
      }
    }
  }
}

void register_packet(conn_group_t *g, conn_t *c, int32_t sn) {
  // store the sequence numbers in BE, as they're transmitted over the network
  c->recv_log[c->recv_idx++] = htobe32(sn);

  if (c->recv_idx == RECV_ACK_INT) {
    srtla_ack_pkt ack;
    ack.type = htobe32(SRTLA_TYPE_ACK << 16);
    memcpy(&ack.acks, &c->recv_log, sizeof(c->recv_log));

    int ret = SENDTO(srtla_sock, &ack, sizeof(ack), 0, (struct sockaddr*)&c->addr, sizeof(struct sockaddr_storage));
    if (ret != sizeof(ack)) {
      err("%s:%d (group %p): failed to send the srtla ack\n",
          print_addr((struct sockaddr*)&c->addr), port_no((struct sockaddr*)&c->addr), g);
    }

    c->recv_idx = 0;
  }
}

void handle_srtla_data(time_t ts) {
  char buf[MTU];
  int ret;

  // Get the packet
  struct sockaddr_storage srtla_addr = {0};
  socklen_t len = sizeof(srtla_addr);
  int n = recvfrom(srtla_sock, buf, MTU, 0, (struct sockaddr*)&srtla_addr, &len);
  if (n < 0) {
    err("Failed to read a srtla packet (err=%s)\n", sock_err_str());
    return;
  }

  // Handle srtla registration packets
  if (is_srtla_reg1(buf, n)) {
    info("Received REG1 from %s:%d (size %d bytes)\n", print_addr((struct sockaddr*)&srtla_addr), port_no((struct sockaddr*)&srtla_addr), n);
    group_reg((struct sockaddr*)&srtla_addr, buf, ts);
    return;
  }

  if (is_srtla_reg2(buf, n)) {
    info("Received REG2 from %s:%d (size %d bytes)\n", print_addr((struct sockaddr*)&srtla_addr), port_no((struct sockaddr*)&srtla_addr), n);
    conn_reg((struct sockaddr*)&srtla_addr, buf, ts);
    return;
  }

  // Check that the peer is a member of a connection group, discard otherwise
  conn_t *c;
  conn_group_t *g;
  ret = group_find_by_addr((struct sockaddr*)&srtla_addr, &g, &c);
  if (ret != 1) {
    static time_t last_log = 0;
    if (ts > last_log + 5) {
      info("Discarding non-SRTLA packet from %s:%d (size %d bytes)\n", print_addr((struct sockaddr*)&srtla_addr), port_no((struct sockaddr*)&srtla_addr), n);
      last_log = ts;
    }
    return;
  }

  // Update the connection's use timestamp and bytes received
  c->last_rcvd = ts;
  c->bytes_received += n;
  g->bytes_received += n;

  // Resend SRTLA keep-alive packets to the sender
  if (is_srtla_keepalive(buf, n)) {
    int ret = SENDTO(srtla_sock, &buf, n, 0, (struct sockaddr*)&srtla_addr, sizeof(struct sockaddr_storage));
    if (ret != n) {
      err("%s:%d (group %p): failed to send the srtla keepalive\n",
          print_addr((struct sockaddr*)&srtla_addr), port_no((struct sockaddr*)&srtla_addr), g);
    }
    return;
  }

  // Check that the packet is large enough to be an SRT packet, discard otherwise
  if (n < SRT_MIN_LEN) return;

  // Record the most recently active peer
  memcpy(&g->last_addr, &srtla_addr, sizeof(struct sockaddr_storage));

  // Keep track of the received data packets to send SRTLA ACKs
  int32_t sn = get_srt_sn(buf, n);
  if (sn >= 0) {
    register_packet(g, c, sn);
  }

  // Open a connection to the SRT server for the group
  if (g->srt_sock < 0) {
    int sock = create_udp_socket();
    if (sock < 0) {
      err("Group #%llu: failed to create an SRT socket (%s)\n", (unsigned long long)g->logical_group_id, sock_err_str());
      if (flag_auto_reconnect) {
        g->state = G_WAITING_SRT;
        g->srt_retry_attempts++;
        uint64_t now = 0; get_ms(&now);
        int backoff = min(flag_reconnect_interval_ms << (g->srt_retry_attempts - 1), REG_RETRY_MAX_MS);
        g->next_srt_retry_ms = now + backoff;
        return;
      }
      group_destroy(g, NULL);
      return;
    }
    g->srt_sock = sock;

    int ret = connect(sock, (struct sockaddr*)&srt_addr, sizeof(struct sockaddr_storage));
    if (ret != 0) {
      err("Group #%llu: failed to connect() the SRT socket (%s)\n", (unsigned long long)g->logical_group_id, sock_err_str());
      close_socket(sock);
      g->srt_sock = -1;
      if (flag_auto_reconnect) {
        g->state = G_WAITING_SRT;
        g->srt_retry_attempts++;
        uint64_t now = 0; get_ms(&now);
        int backoff = min(flag_reconnect_interval_ms << (g->srt_retry_attempts - 1), REG_RETRY_MAX_MS);
        g->next_srt_retry_ms = now + backoff;
        return;
      }
      group_destroy(g, NULL);
      return;
    }

#ifdef __linux__
    ret = epoll_add(sock, EPOLLIN, g);
    if (ret < 0) {
      err("Group #%llu: failed to add the SRT socket to the epoll\n", (unsigned long long)g->logical_group_id);
      close_socket(sock);
      g->srt_sock = -1;
      if (flag_auto_reconnect) {
        g->state = G_WAITING_SRT;
        uint64_t now = 0; get_ms(&now);
        g->next_srt_retry_ms = now + flag_reconnect_interval_ms;
        return;
      }
      group_destroy(g, NULL);
      return;
    }
#endif
  }

  ret = send(g->srt_sock, buf, n, 0);
  if (ret != n) {
    err("Group %p: failed to forward the srtla packet, terminating the group\n", g);
    group_destroy(g, NULL);
  }
}

/*
  Freeing resources

  Groups:
    * new groups with no connection: created_at < (ts - G_TIMEOUT)
    * other groups: when all connections have timed out
  Connections:
    * GC last_rcvd < (ts - CONN_TIMEOUT)
*/
void connection_cleanup(time_t ts) {
  static time_t last_ran = 0;
  if ((last_ran + CLEANUP_PERIOD) > ts) return;
  last_ran = ts;

  if (groups == NULL) return;

  int total_groups = 0, total_conns = 0, removed_groups = 0, removed_conns = 0;

  debug("Started a cleanup run\n");

  conn_group_t *next_g = NULL;
  conn_group_t **prev_g = &groups;
  for (conn_group_t *g = groups; g != NULL; g = next_g) {
    total_groups++;
    next_g = g->next;

    conn_t *next_c = NULL;
    conn_t **prev_c = &g->conns;
    for (conn_t *c = g->conns; c != NULL; c = next_c) {
      total_conns++;
      next_c = c->next;
      if ((c->last_rcvd + CONN_TIMEOUT) < ts) {
        removed_conns++;
        info("%s:%d (group %p): connection removed (timed out)\n",
             print_addr((struct sockaddr*)&c->addr), port_no((struct sockaddr*)&c->addr), g);
        *prev_c = next_c;
#ifdef _WIN32
        InterlockedDecrement(&srtla_active_connections);
        InterlockedIncrement(&srtla_failed_connections);
#endif
        free(c);
        continue;
      }
      prev_c = &c->next;
    }

    if (g->conns == NULL && (g->created_at + GROUP_TIMEOUT) < ts) {
      removed_groups++;
      info("Group %p: removed (no connections)\n", g);
      group_destroy(g, prev_g);
      continue;
    }
    prev_g = &g->next;
  }

  /* Also attempt SRT re-handshake for groups in WAITING_SRT state */
  for (conn_group_t *g = groups; g != NULL; g = g->next) {
    if (g->state == G_WAITING_SRT) {
      uint64_t now_ms = 0;
      uint64_t tmp = 0;
      if (get_ms(&tmp) == 0) now_ms = tmp;
      if (now_ms >= (uint64_t)g->next_srt_retry_ms) {
        // Try to handshake with the existing srt_addr
        info("Group #%llu: retrying SRT handshake attempt %d\n", (unsigned long long)g->logical_group_id, g->srt_retry_attempts);
        srt_handshake_t hs_packet = {0};
        hs_packet.header.type = htobe16(SRT_TYPE_HANDSHAKE);
        hs_packet.version = htobe32(4);
        hs_packet.ext_field = htobe16(2);
        hs_packet.handshake_type = htobe32(1);

        int sock = create_udp_socket();
        if (sock >= 0) {
#ifdef _WIN32
          DWORD to = 1000;
          setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof(to));
#else
          struct timeval to = { .tv_sec = 1, .tv_usec = 0 };
          setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof(to));
#endif
          if (connect(sock, (struct sockaddr*)&srt_addr, sizeof(struct sockaddr_storage)) == 0) {
            int sent = send(sock, (const char*)&hs_packet, sizeof(hs_packet), 0);
            if (sent == sizeof(hs_packet)) {
              char buf[MTU];
              int r = recv(sock, buf, MTU, 0);
              if (r == sizeof(hs_packet)) {
                // success
                g->srt_sock = sock;
                g->state = G_ACTIVE;
                g->srt_retry_attempts = 0;
                info("Group #%llu: SRT handshake succeeded, group ACTIVE\n", (unsigned long long)g->logical_group_id);
                // continue to next group
                continue;
              }
            }
          }
          close_socket(sock);
        }

        if (g->state != G_ACTIVE) {
          g->srt_retry_attempts++;
          int backoff = REG_RETRY_BASE_MS << (g->srt_retry_attempts - 1);
          if (backoff > REG_RETRY_MAX_MS) backoff = REG_RETRY_MAX_MS;
          uint64_t now = 0;
          get_ms(&now);
          g->next_srt_retry_ms = now + backoff;
          info("Group #%llu: scheduling next SRT retry in %d ms\n", (unsigned long long)g->logical_group_id, backoff);
        }
      }
    }
  }

  debug("Clean up run ended. Counted %d groups and %d connections. "
        "Removed %d groups and %d connections\n",
        total_groups, total_conns, removed_groups, removed_conns);
}

/*
SRT is connection-oriented and it won't reply to our packets at this point
unless we start a handshake, so we do that for each resolved address

Returns: -1 when an error has been encountered
          0 when the address was resolved but SRT appears unreachable
          1 when the address was resolved and SRT appears reachable
*/
int resolve_srt_addr(char *host, char *port, volatile int *stop_flag) {
  // Let's set up an SRT handshake induction packet
  srt_handshake_t hs_packet = {0};
  hs_packet.header.type = htobe16(SRT_TYPE_HANDSHAKE);
  hs_packet.version = htobe32(4);
  hs_packet.ext_field = htobe16(2);
  hs_packet.handshake_type = htobe32(1);

  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  struct addrinfo *srt_addrs;
  int ret = getaddrinfo(host, port, &hints, &srt_addrs);
  if (ret != 0) {
    fprintf(stderr, "Failed to resolve the address %s:%s\n", host, port);
    return -1;
  }

  int tmp_sock = (int)socket(srt_addrs->ai_family, SOCK_DGRAM, 0);
  if (tmp_sock < 0) {
    perror("failed to create a UDP socket");
    return -1;
  }

  int found = -1;
  for (struct addrinfo *addr = srt_addrs; addr != NULL && found == -1; addr = addr->ai_next) {
    info("Trying to connect to SRT at %s:%s... ", print_addr(addr->ai_addr), port);
    /* We're not printing this at all log levels, but a
       flush won't hurt if we didn't print anything */
    fflush(stderr);
    ret = connect(tmp_sock, addr->ai_addr, (int)addr->ai_addrlen);
    if (ret == 0) {
#ifdef _WIN32
      ret = send(tmp_sock, (const char*)&hs_packet, sizeof(hs_packet), 0);
#else
      ret = send(tmp_sock, &hs_packet, sizeof(hs_packet), 0);
#endif
      if (ret == sizeof(hs_packet)) {
        char buf[MTU];
        int waited = 0;
        while (waited < 1000) {
          if (stop_flag && *stop_flag) {
            close_socket(tmp_sock);
            freeaddrinfo(srt_addrs);
            return -1;
          }
          fd_set readfds;
          FD_ZERO(&readfds);
          FD_SET(tmp_sock, &readfds);
          struct timeval tv = {0, 50000}; // 50ms
          int ready = select(tmp_sock + 1, &readfds, NULL, NULL, &tv);
          if (ready > 0) {
            ret = recv(tmp_sock, buf, MTU, 0);
            if (ret == sizeof(hs_packet)) {
              info("success\n");
              memcpy(&srt_addr, addr->ai_addr, addr->ai_addrlen);
              found = 1;
            }
            break;
          }
          waited += 50;
        }
      } // ret == sizeof(buf)
    } // ret == 0

    if (found == -1) {
      info("error\n");
    }
  }
  close_socket(tmp_sock);

  if (found == -1) {
    memcpy(&srt_addr, srt_addrs->ai_addr, srt_addrs->ai_addrlen);
    fprintf(stderr, "WARNING: Failed to confirm that a SRT server is reachable at any address\n"
                    "Proceeding with the first address %s\n", print_addr((struct sockaddr*)&srt_addr));
    found = 0;
  }

  freeaddrinfo(srt_addrs);

  return found;
}

#ifdef _WIN32
static ULONG get_interface_index_for_ip(const char *ip_str) {
  ULONG if_index = 0;
  struct in_addr dest_ip;
  if (inet_pton(AF_INET, ip_str, &dest_ip) <= 0) return 0;

  // First try GetBestInterface
  if (GetBestInterface(dest_ip.s_addr, &if_index) == NO_ERROR) {
    return if_index;
  }

  // Fallback: GetAdaptersAddresses
  ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
  ULONG buf_size = 15000;
  PIP_ADAPTER_ADDRESSES addresses = (PIP_ADAPTER_ADDRESSES)malloc(buf_size);
  if (addresses == NULL) return 0;

  DWORD ret = GetAdaptersAddresses(AF_INET, flags, NULL, addresses, &buf_size);
  if (ret == ERROR_BUFFER_OVERFLOW) {
    free(addresses);
    addresses = (PIP_ADAPTER_ADDRESSES)malloc(buf_size);
    if (addresses == NULL) return 0;
    ret = GetAdaptersAddresses(AF_INET, flags, NULL, addresses, &buf_size);
  }

  if (ret == NO_ERROR) {
    for (PIP_ADAPTER_ADDRESSES addr = addresses; addr != NULL; addr = addr->Next) {
      for (PIP_ADAPTER_UNICAST_ADDRESS unicast = addr->FirstUnicastAddress; unicast != NULL; unicast = unicast->Next) {
        if (unicast->Address.lpSockaddr->sa_family == AF_INET) {
          struct sockaddr_in *sin = (struct sockaddr_in *)unicast->Address.lpSockaddr;
          if (sin->sin_addr.s_addr == dest_ip.s_addr) {
            if_index = addr->IfIndex;
            break;
          }
        }
      }
      if (if_index != 0) break;
    }
  }

  free(addresses);
  return if_index;
}
#endif

int srtla_rec_main(const char *listen_ip, int srtla_port, const char *srt_host, int srt_port, volatile int *stop_flag) {
  srtla_ctx_t *ctx = calloc(1, sizeof(srtla_ctx_t));
  if (!ctx) return -1;
  ctx->_srtla_sock = -1;
  ctx->_listen_port = srtla_port;
  ctx->_group_seq = 1;
  
  pthread_mutex_lock(&global_ctx_mutex);
  int ctx_idx = -1;
  for (int i = 0; i < MAX_SRTLA_INSTANCES; i++) {
      if (!global_contexts[i]) {
          global_contexts[i] = ctx;
          ctx_idx = i;
          break;
      }
  }
  pthread_mutex_unlock(&global_ctx_mutex);
  
  if (ctx_idx == -1) {
      err("Maximum SRTLA instances reached!");
      free(ctx);
      return -1;
  }
  
  current_ctx = ctx;
  
  struct sockaddr_in listen_addr;
  memset(&listen_addr, 0, sizeof(listen_addr));

  char srt_port_str[16];
  snprintf(srt_port_str, sizeof(srt_port_str), "%d", srt_port);
  
  // Try to detect if the SRT server is reachable.
  int ret = resolve_srt_addr((char*)srt_host, srt_port_str, stop_flag);
  if (ret < 0) {
    return -1;
  }

#ifdef _WIN32
  // Windows'ta urandom yerine CryptGenRandom kullanacağız, bu değişkene ihtiyaç yok
#else
  // urandom is used to generate random ids
  urandom = fopen("/dev/urandom", "rb");
  if (urandom == NULL) {
    perror("failed to open urandom\n");
    return -1;
  }
#endif

#ifdef __linux__
  // We use epoll for event-driven network I/O
  socket_epoll = epoll_create(1000); // the number is ignored since Linux 2.6.8
  if (socket_epoll < 0) {
    perror("epoll_create");
    return -1;
  }
#endif

  // Set up the listener socket for incoming SRT connections
  listen_addr.sin_family = AF_INET;
  if (listen_ip && *listen_ip) {
    if (inet_pton(AF_INET, listen_ip, &listen_addr.sin_addr) <= 0) {
      err("Invalid listen IP address format '%s', binding to INADDR_ANY instead.\n", listen_ip);
      listen_addr.sin_addr.s_addr = INADDR_ANY;
    }
  } else {
    listen_addr.sin_addr.s_addr = INADDR_ANY;
  }
  listen_addr.sin_port = htons(srtla_port);
  srtla_sock = (int)socket(AF_INET, SOCK_DGRAM, 0);
  if (srtla_sock < 0) {
    err("socket creation failed: %s\n", sock_err_str());
    srtla_sock = -1;
    return -1;
  }
  
#ifdef _WIN32
  #ifndef IP_UNICAST_IF
  #define IP_UNICAST_IF 31
  #endif
  if (listen_ip && *listen_ip) {
    DWORD if_index = get_interface_index_for_ip(listen_ip);
    if (if_index != 0) {
      DWORD if_index_nbo = htonl(if_index);
      if (setsockopt(srtla_sock, IPPROTO_IP, IP_UNICAST_IF, (const char *)&if_index_nbo, sizeof(if_index_nbo)) == SOCKET_ERROR) {
        err("Failed to set IP_UNICAST_IF to interface index %lu (%s)\n", if_index, sock_err_str());
      } else {
        info("Bound srtla_sock outbound routing to network interface index %lu (NBO: 0x%08X)\n", if_index, (unsigned int)if_index_nbo);
      }
    } else {
      err("Failed to find interface index for local IP %s\n", listen_ip);
    }
  }

  #ifndef SIO_UDP_CONNRESET
  #define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
  #endif
  BOOL bNewBehavior = FALSE;
  DWORD dwBytesReturned = 0;
  WSAIoctl(srtla_sock, SIO_UDP_CONNRESET, &bNewBehavior, sizeof(bNewBehavior), NULL, 0, &dwBytesReturned, NULL, NULL);
#endif

  ret = bind(srtla_sock, (const struct sockaddr *)&listen_addr, sizeof(struct sockaddr_in));
  if (ret < 0) {
    err("bind failed on port %d: %s\n", srtla_port, sock_err_str());
    close_socket(srtla_sock);
    srtla_sock = -1;
    return -1;
  }

#ifdef __linux__
  ret = epoll_add(srtla_sock, EPOLLIN, NULL);
  if (ret != 0) {
    err("failed to add the srtla sock to the epoll\n");
    close_socket(srtla_sock);
    srtla_sock = -1;
    return -1;
  }
#endif

  info("srtla_rec is now running\n");
  current_ctx->_is_listening = true;

  while(stop_flag && !(*stop_flag)) {
#ifdef __linux__
    #define MAX_EPOLL_EVENTS 10
    struct epoll_event events[MAX_EPOLL_EVENTS];
    int eventcnt = epoll_wait(socket_epoll, events, MAX_EPOLL_EVENTS, 1000);

    time_t ts = 0;
    int ret = get_seconds(&ts);
    if (ret != 0) {
      err("Failed to get the timestamp\n");
    }
    int group_cnt;
    for (int i = 0; i < eventcnt; i++) {
      group_cnt = group_count;
      if (events[i].data.ptr == NULL) {
        handle_srtla_data(ts);
      } else {
        handle_srt_data((conn_group_t*)events[i].data.ptr);
      }
      if (group_count < group_cnt) break;
    }
    connection_cleanup(ts);
#else
    time_t ts = 0;
    int ret = get_seconds(&ts);
    if (ret != 0) {
      err("Failed to get the timestamp\n");
    }
    // Windows için select() ile hem srtla_sock hem de tüm aktif SRT socket'lerini dinle
    fd_set readfds;
    FD_ZERO(&readfds);
    int maxfd = srtla_sock;
    FD_SET(srtla_sock, &readfds);
    for (conn_group_t *g = groups; g != NULL; g = g->next) {
      if (g->srt_sock > 0) {
        FD_SET(g->srt_sock, &readfds);
        if (g->srt_sock > maxfd) maxfd = g->srt_sock;
      }
    }
    struct timeval tv = {0, 100000}; // 100ms
    __try {
      int ready = select(maxfd + 1, &readfds, NULL, NULL, &tv);
      if (ready > 0) {
        if (FD_ISSET(srtla_sock, &readfds)) {
          handle_srtla_data(ts);
        }
        for (conn_group_t *g = groups; g != NULL; g = g->next) {
          if (g->srt_sock > 0 && FD_ISSET(g->srt_sock, &readfds)) {
            handle_srt_data(g);
          }
        }    
      }
      connection_cleanup(ts);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
      err("SEH EXCEPTION in srtla_rec_main loop! Ignoring to prevent OBS crash.");
    }
#endif
  } // while loop end
  current_ctx->_is_listening = false;

  if (srtla_sock >= 0) {
    close_socket(srtla_sock);
    srtla_sock = -1;
  }

  // Group cleanup
  conn_group_t *g = groups;
  while (g) {
    conn_group_t *next = g->next;
    group_destroy(g, NULL);
    g = next;
  }
  groups = NULL;
  group_count = 0;
  srtla_active_connections = 0;
  
  pthread_mutex_lock(&global_ctx_mutex);
  global_contexts[ctx_idx] = NULL;
  pthread_mutex_unlock(&global_ctx_mutex);
  
  free(ctx);
  current_ctx = NULL;
  
  return 0;
}

#include <stdbool.h>
void srtla_get_connection_stats(bool *is_listening, int *active_groups, int *active_connections) {
    *is_listening = false;
    *active_groups = 0;
    *active_connections = 0;
    
    pthread_mutex_lock(&global_ctx_mutex);
    for (int i = 0; i < MAX_SRTLA_INSTANCES; i++) {
        if (global_contexts[i]) {
            if (global_contexts[i]->_is_listening) {
                *is_listening = true;
                *active_groups += global_contexts[i]->_group_count;
            }
            *active_connections += global_contexts[i]->_active_connections;
        }
    }
    pthread_mutex_unlock(&global_ctx_mutex);
}

void srtla_get_connection_details(int *listen_port, int *failed_conns, char* out_buffer, int max_len) {
    *listen_port = 0; // Deprecated single port
    *failed_conns = 0;
    int offset = 0;
    
    if (out_buffer && max_len > 0) {
        offset += snprintf(out_buffer + offset, max_len - offset, "{");
        
        pthread_mutex_lock(&global_ctx_mutex);
        
        // Add active ports array
        offset += snprintf(out_buffer + offset, max_len - offset, "\"ports\":[");
        bool first_port = true;
        for (int i = 0; i < MAX_SRTLA_INSTANCES; i++) {
            srtla_ctx_t *ctx = global_contexts[i];
            if (ctx && ctx->_is_listening) {
                if (!first_port) offset += snprintf(out_buffer + offset, max_len - offset, ",");
                first_port = false;
                offset += snprintf(out_buffer + offset, max_len - offset, "%d", ctx->_listen_port);
                *listen_port = ctx->_listen_port; // Set legacy int to the last port found
            }
        }
        offset += snprintf(out_buffer + offset, max_len - offset, "],");
        
        // Add groups array
        offset += snprintf(out_buffer + offset, max_len - offset, "\"groups\":[");
        bool first_group = true;
        for (int i = 0; i < MAX_SRTLA_INSTANCES; i++) {
            srtla_ctx_t *ctx = global_contexts[i];
            if (ctx) {
                *failed_conns += ctx->_failed_connections;
#ifdef _WIN32
                __try {
#endif
                conn_group_t *g = ctx->_groups;
                while (g && offset < max_len - 1) {
                    if (!first_group) offset += snprintf(out_buffer + offset, max_len - offset, ",");
                    first_group = false;
                    offset += snprintf(out_buffer + offset, max_len - offset, "{\"id\":%llu,\"bytes\":%llu,\"listen_port\":%d,\"conns\":[", 
                        (unsigned long long)g->logical_group_id, (unsigned long long)g->bytes_received, ctx->_listen_port);
                    
                    conn_t *c = g->conns;
                    while (c && offset < max_len - 1) {
                        offset += snprintf(out_buffer + offset, max_len - offset, "{\"ip\":\"%s\",\"port\":%d,\"bytes\":%llu}", 
                            print_addr((struct sockaddr*)&c->addr), port_no((struct sockaddr*)&c->addr), (unsigned long long)c->bytes_received);
                        c = c->next;
                        if (c) offset += snprintf(out_buffer + offset, max_len - offset, ",");
                    }
                    offset += snprintf(out_buffer + offset, max_len - offset, "]}");
                    g = g->next;
                }
#ifdef _WIN32
                } __except(EXCEPTION_EXECUTE_HANDLER) {}
#endif
            }
        }
        pthread_mutex_unlock(&global_ctx_mutex);
        snprintf(out_buffer + offset, max_len - offset, "]}");
    }
}

