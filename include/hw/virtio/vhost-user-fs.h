/*
 * Vhost-user filesystem virtio device
 *
 * Copyright 2018-2019 Red Hat, Inc.
 *
 * Authors:
 *  Stefan Hajnoczi <stefanha@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or
 * (at your option) any later version.  See the COPYING file in the
 * top-level directory.
 */

#ifndef QEMU_VHOST_USER_FS_H
#define QEMU_VHOST_USER_FS_H

#include "hw/virtio/virtio.h"
#include "hw/virtio/vhost.h"
#include "hw/virtio/vhost-user.h"
#include "chardev/char-fe.h"
#include "qom/object.h"

#define TYPE_VHOST_USER_FS "vhost-user-fs-device"
OBJECT_DECLARE_SIMPLE_TYPE(VHostUserFS, VHOST_USER_FS)

/* Max entries in one virtio-fs backend request back to QEMU */
#define VHOST_USER_FS_BACKEND_ENTRIES 8

/* For the flags field of VhostUserFSBackendMsg */
#define VHOST_USER_FS_FLAG_MAP_R (1u << 0)
#define VHOST_USER_FS_FLAG_MAP_W (1u << 1)

/* Backend request message to update the MMIO window */
typedef struct {
    /* File offset */
    uint64_t fd_offset[VHOST_USER_FS_BACKEND_ENTRIES];
    /* Offset into the DAX window */
    uint64_t cache_offset[VHOST_USER_FS_BACKEND_ENTRIES];
    /* Size of region to map */
    uint64_t len[VHOST_USER_FS_BACKEND_ENTRIES];
    /* Flags for the mmap operation, from VHOST_USER_FS_FLAG_* */
    uint64_t flags[VHOST_USER_FS_BACKEND_ENTRIES];
} VhostUserFSBackendMsg;

typedef struct {
    CharBackend chardev;
    char *tag;
    uint16_t num_request_queues;
    uint16_t queue_size;
    uint64_t cache_size;
} VHostUserFSConf;

struct VHostUserFS {
    /*< private >*/
    VirtIODevice parent;
    VHostUserFSConf conf;
    struct vhost_virtqueue *vhost_vqs;
    struct vhost_dev vhost_dev;
    VhostUserState vhost_user;
    VirtQueue **req_vqs;
    VirtQueue *hiprio_vq;
    int32_t bootindex;

    /*< public >*/
    MemoryRegion cache;
};

/* Callbacks from the vhost-user code for backend commands */
int vhost_user_fs_backend_map(struct vhost_dev *dev,
                              const VhostUserFSBackendMsg *msg, int fd);
int vhost_user_fs_backend_unmap(struct vhost_dev *dev,
                                const VhostUserFSBackendMsg *msg);

#endif /* QEMU_VHOST_USER_FS_H */
