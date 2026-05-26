/*
 * If not stated otherwise in this file or this component's LICENSE file the
 * following copyright and licenses apply:
 *
 * Copyright 2026 RDK Management
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef AVSYNC_UDS_SERVER_H
#define AVSYNC_UDS_SERVER_H

/**
 * Vendor-private AVSync UDS session server.
 *
 * This module implements the server role for the AVSync UDS descriptor exchange
 * handshake (SCM_RIGHTS FD passing) described by the AVClock shm+eventfd design.
 *
 * Intended usage:
 *  - AVClock process (or broker) starts the UDS server
 *  - AVClock process sets/clears the "active session" for a given clockId
 *  - Sink processes connect as clients and call GET_SESSION / DETACH
 *
 * The server is responsible for:
 *  - replying to GET_SESSION with the active session descriptor
 *  - returning the shm FD and per-consumer eventfd via SCM_RIGHTS
 *  - tracking consumer eventfds so the AVClock side can notify them
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#include "avsync/avsync_ipc_protocol.h"

typedef struct AvSyncUdsServer AvSyncUdsServer;

/**
 * Optional authorization callback.
 *
 * The protocol document requires authentication/authorization (e.g. SO_PEERCRED).
 * This server provides a hook so products can enforce policy.
 *
 * Return true to allow, false to deny.
 */
typedef bool (*avsync_uds_server_auth_cb)(int peer_pid,
                                         int peer_uid,
                                         int peer_gid,
                                         void* user_data);

typedef struct AvSyncUdsServerConfig {
  const char* uds_path; /* NULL/empty => AVSYNC_UDS_DEFAULT_PATH. '@' => abstract namespace */
  avsync_uds_server_auth_cb auth_cb; /* optional */
  void* auth_user_data;              /* optional */
} AvSyncUdsServerConfig;

typedef struct AvSyncUdsServerSessionInfo {
  uint32_t clock_id;
  uint64_t session_id;
  int shm_fd;               /* server will dup() and store its own FD */
  uint32_t shm_size_bytes;
  uint32_t shm_layout_version;
  uint32_t flags;
} AvSyncUdsServerSessionInfo;

/* PUBLIC_INTERFACE */
int avsync_uds_server_create(const AvSyncUdsServerConfig* cfg, AvSyncUdsServer** out_server);
/**
 * Create a server instance. Does not start listening until avsync_uds_server_start().
 *
 * @return 0 on success, <0 on local error.
 */

/* PUBLIC_INTERFACE */
void avsync_uds_server_destroy(AvSyncUdsServer* server);
/**
 * Stop the server if running and free resources.
 */

/* PUBLIC_INTERFACE */
int avsync_uds_server_start(AvSyncUdsServer* server);
/**
 * Start the background thread and begin accepting UDS connections.
 *
 * @return 0 on success, <0 on local error.
 */

/* PUBLIC_INTERFACE */
int avsync_uds_server_stop(AvSyncUdsServer* server);
/**
 * Request server shutdown and join the background thread.
 *
 * @return 0 on success, <0 on local error.
 */

/* PUBLIC_INTERFACE */
int avsync_uds_server_set_active_session(AvSyncUdsServer* server,
                                        const AvSyncUdsServerSessionInfo* session);
/**
 * Register/replace the currently active session for a given clock_id.
 *
 * Note: The server dup()s the provided shm_fd and owns the duplicated FD until
 * replaced/cleared or server is destroyed.
 *
 * @return 0 on success, <0 on local error.
 */

/* PUBLIC_INTERFACE */
int avsync_uds_server_clear_active_session(AvSyncUdsServer* server,
                                          uint32_t clock_id,
                                          uint64_t session_id);
/**
 * Clear the active session and drop all tracked consumer attachments for it.
 *
 * @return 0 on success, <0 on local error.
 */

/* PUBLIC_INTERFACE */
int avsync_uds_server_notify_consumers(AvSyncUdsServer* server,
                                       uint32_t clock_id,
                                       uint64_t session_id);
/**
 * Write a wakeup (uint64_t 1) to every eventfd currently attached to the given
 * (clock_id, session_id). The shm update (including flags) is the caller's
 * responsibility; eventfd is only a wakeup primitive.
 *
 * @return number of consumers notified (>=0) on success, <0 on error.
 */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* AVSYNC_UDS_SERVER_H */
