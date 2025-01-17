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

#include "qemu/osdep.h"
#include <sys/ioctl.h>
#include "standard-headers/linux/virtio_fs.h"
#include "qapi/error.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "hw/virtio/virtio-bus.h"
#include "hw/virtio/virtio-access.h"
#include "qemu/error-report.h"
#include "hw/virtio/vhost.h"
#include "hw/virtio/vhost-user-fs.h"
#include "monitor/monitor.h"
#include "sysemu/sysemu.h"

static const int user_feature_bits[] = {
    VIRTIO_F_VERSION_1,
    VIRTIO_RING_F_INDIRECT_DESC,
    VIRTIO_RING_F_EVENT_IDX,
    VIRTIO_F_NOTIFY_ON_EMPTY,
    VIRTIO_F_RING_PACKED,
    VIRTIO_F_IOMMU_PLATFORM,
    VIRTIO_F_RING_RESET,

    VHOST_INVALID_FEATURE_BIT
};

/*
 * The powerpc kernel code expects the memory to be accessible during
 * addition/removal.
 */
#if defined(TARGET_PPC64) && defined(CONFIG_LINUX)
#define DAX_WINDOW_PROT PROT_READ
#else
#define DAX_WINDOW_PROT PROT_NONE
#endif

/* Outputs debug information about a backend message to stdout */
static void debug_backend_msg(const char *desc,
                              const VhostUserFSBackendMsg *msg, const int* fd)
{
    unsigned int i;
    bool flag_seen = false;
    uint64_t e_fd_offset, e_cache_offset, e_len, e_flags;

    /* Output description */
    if (desc != NULL) {
        printf("%s", desc);
    }
    if (fd) {
        printf(" (fd=%d)", *fd);
    }
    printf(":\n");

    /* Output message content */
    for (i = 0; i < VHOST_USER_FS_BACKEND_ENTRIES; ++i) {
        e_len = msg->len[i];
        if (!e_len) {
            continue;
        }
        e_fd_offset = msg->fd_offset[i];
        e_cache_offset = msg->cache_offset[i];
        e_flags = msg->flags[i];

        printf("[%d]: fd_offset=0x%" PRIx64 ", cache_offset=0x%" PRIx64
               ", len=0x%" PRIx64 ", flags=",
               i, e_fd_offset, e_cache_offset, e_len);
        if (e_flags & VHOST_USER_FS_FLAG_MAP_R) {
            printf("MAP_R");
            e_flags &= ~VHOST_USER_FS_FLAG_MAP_R;
            flag_seen = true;
        }
        if (e_flags & VHOST_USER_FS_FLAG_MAP_W) {
            if (flag_seen) {
                printf("|");
            }
            printf("MAP_W");
            e_flags &= ~VHOST_USER_FS_FLAG_MAP_W;
            flag_seen = true;
        }
        if (e_flags) {
            if (flag_seen) {
                printf("|");
            }
            printf("0x%lx", e_flags);
            flag_seen = true;
        }
        if (!flag_seen) {
            printf("EMPTY");
        }
        printf("\n");
    }
}

/*
 * Writes a pointer to the DAX cache (in host virtual address space) to
 * *cache_host and the cache's size to *cache_size.
 */
static int get_cache(struct vhost_dev *dev,
                     void **cache_host, size_t *cache_size)
{
    VHostUserFS *fs = VHOST_USER_FS(dev->vdev);
    *cache_size = fs->conf.cache_size;
    if (!cache_size) {
        error_report("map/unmap called when DAX cache not present");
        return -ENOENT;
    }
    *cache_host = memory_region_get_ram_ptr(&fs->cache);
    return 0;
}

/*
 * Carries out the map operations from msg and returns on the first error.
 * cache_host must be a pointer to the DAX cache in host virtual address space.
 */
static int map_in_cache(void *cache_host, size_t cache_size,
                        const VhostUserFSBackendMsg *msg, int fd)
{
    int i, res = 0;
    uint64_t e_fd_offset, e_cache_offset, e_len, e_flags;
    void *ptr;

    if (fd < 0) {
        error_report("map called with bad FD");
        return -EBADF;
    }

    for (i = 0; i < VHOST_USER_FS_BACKEND_ENTRIES; ++i) {
        e_len = msg->len[i];
        if (!e_len) {
            continue;
        }
        e_fd_offset = msg->fd_offset[i];
        e_cache_offset = msg->cache_offset[i];
        e_flags = msg->flags[i];

        if ((e_cache_offset + e_len) < e_len /* <- checks for overflow */ ||
            (e_cache_offset + e_len) > cache_size) {
            res = -EINVAL;
            error_report("map [%d]: bad cache_offset+len 0x%" PRIx64 "+0x%"
                         PRIx64,
                         i, e_cache_offset, e_len);
            break;
        }

        ptr = mmap(cache_host + e_cache_offset, e_len,
                   ((e_flags & VHOST_USER_FS_FLAG_MAP_R) ? PROT_READ : 0) |
                   ((e_flags & VHOST_USER_FS_FLAG_MAP_W) ? PROT_WRITE : 0),
                   MAP_SHARED | MAP_FIXED, fd, e_fd_offset);

        if (ptr != cache_host + e_cache_offset) {
            res = -errno;
            error_report("map [%d] failed with error %s", i, strerror(errno));
            break;
        }
    }

    return res;
}

/*
 * Carries out the unmap operations from in msg. On error, the remaining
 * operations are tried anyway. Only the last error is returned.
 * cache_host must be a pointer to the DAX cache in host virtual address space.
 */
static int unmap_in_cache(void *cache_host, size_t cache_size,
                          const VhostUserFSBackendMsg *msg)
{
    int i, res = 0;
    uint64_t e_cache_offset, e_len;
    void *ptr;

    /*
     * Note even if one unmap fails we try the rest, since the intended effect
     * is to clean up as much as possible.
     */
    for (i = 0; i < VHOST_USER_FS_BACKEND_ENTRIES && msg->len[i]; ++i) {
        e_len = msg->len[i];
        if (!e_len) {
            continue;
        }
        e_cache_offset = msg->cache_offset[i];

        if (e_len == ~(uint64_t)0) {
            /* Special case meaning the whole arena */
            e_len = cache_size;
        }

        if ((e_cache_offset + e_len) < e_len /* <- checks for overflow */ ||
            (e_cache_offset + e_len) > cache_size) {
            res = -EINVAL;
            error_report("unmap [%d]: bad cache_offset+len 0x%" PRIx64 "+0x%"
                         PRIx64,
                         i, e_cache_offset, e_len);
            continue;
        }

        ptr = mmap(cache_host + e_cache_offset, e_len, DAX_WINDOW_PROT,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);

        if (ptr != cache_host + e_cache_offset) {
            res = -errno;
            error_report("unmap [%d]: failed with error %s",
                         i, strerror(errno));
        }
    }

    return res;
}

int vhost_user_fs_backend_map(struct vhost_dev *dev,
                              const VhostUserFSBackendMsg *msg, int fd)
{
    #ifdef DEBUG_VHOST_USER_FS
    debug_backend_msg("vhost_user_fs_backend_map", msg, &fd);
    #endif

    size_t cache_size;
    void *cache_host;
    int res = get_cache(dev, &cache_host, &cache_size);
    if (res) {
        return res;
    }

    res = map_in_cache(cache_host, cache_size, msg, fd);
    if (res) {
        /* Something went wrong, unmap them all */
        unmap_in_cache(cache_host, cache_size, msg);
    }
    return res;
}

int vhost_user_fs_backend_unmap(struct vhost_dev *dev,
                                const VhostUserFSBackendMsg *msg)
{
    #ifdef DEBUG_VHOST_USER_FS
    debug_backend_msg("vhost_user_fs_backend_unmap", msg, NULL);
    #endif

    size_t cache_size;
    void *cache_host;
    int res = get_cache(dev, &cache_host, &cache_size);
    if (res) {
        return res;
    }

    return unmap_in_cache(cache_host, cache_size, msg);
}

static void vuf_get_config(VirtIODevice *vdev, uint8_t *config)
{
    VHostUserFS *fs = VHOST_USER_FS(vdev);
    struct virtio_fs_config fscfg = {};

    memcpy((char *)fscfg.tag, fs->conf.tag,
           MIN(strlen(fs->conf.tag) + 1, sizeof(fscfg.tag)));

    virtio_stl_p(vdev, &fscfg.num_request_queues, fs->conf.num_request_queues);

    memcpy(config, &fscfg, sizeof(fscfg));
}

static void vuf_start(VirtIODevice *vdev)
{
    VHostUserFS *fs = VHOST_USER_FS(vdev);
    BusState *qbus = BUS(qdev_get_parent_bus(DEVICE(vdev)));
    VirtioBusClass *k = VIRTIO_BUS_GET_CLASS(qbus);
    int ret;
    int i;

    if (!k->set_guest_notifiers) {
        error_report("binding does not support guest notifiers");
        return;
    }

    ret = vhost_dev_enable_notifiers(&fs->vhost_dev, vdev);
    if (ret < 0) {
        error_report("Error enabling host notifiers: %d", -ret);
        return;
    }

    ret = k->set_guest_notifiers(qbus->parent, fs->vhost_dev.nvqs, true);
    if (ret < 0) {
        error_report("Error binding guest notifier: %d", -ret);
        goto err_host_notifiers;
    }

    fs->vhost_dev.acked_features = vdev->guest_features;
    ret = vhost_dev_start(&fs->vhost_dev, vdev, true);
    if (ret < 0) {
        error_report("Error starting vhost: %d", -ret);
        goto err_guest_notifiers;
    }

    /*
     * guest_notifier_mask/pending not used yet, so just unmask
     * everything here.  virtio-pci will do the right thing by
     * enabling/disabling irqfd.
     */
    for (i = 0; i < fs->vhost_dev.nvqs; i++) {
        vhost_virtqueue_mask(&fs->vhost_dev, vdev, i, false);
    }

    return;

err_guest_notifiers:
    k->set_guest_notifiers(qbus->parent, fs->vhost_dev.nvqs, false);
err_host_notifiers:
    vhost_dev_disable_notifiers(&fs->vhost_dev, vdev);
}

static void vuf_stop(VirtIODevice *vdev)
{
    VHostUserFS *fs = VHOST_USER_FS(vdev);
    BusState *qbus = BUS(qdev_get_parent_bus(DEVICE(vdev)));
    VirtioBusClass *k = VIRTIO_BUS_GET_CLASS(qbus);
    int ret;

    if (!k->set_guest_notifiers) {
        return;
    }

    vhost_dev_stop(&fs->vhost_dev, vdev, true);

    ret = k->set_guest_notifiers(qbus->parent, fs->vhost_dev.nvqs, false);
    if (ret < 0) {
        error_report("vhost guest notifier cleanup failed: %d", ret);
        return;
    }

    vhost_dev_disable_notifiers(&fs->vhost_dev, vdev);
}

static void vuf_set_status(VirtIODevice *vdev, uint8_t status)
{
    VHostUserFS *fs = VHOST_USER_FS(vdev);
    bool should_start = virtio_device_should_start(vdev, status);

    if (vhost_dev_is_started(&fs->vhost_dev) == should_start) {
        return;
    }

    if (should_start) {
        vuf_start(vdev);
    } else {
        vuf_stop(vdev);
    }
}

static uint64_t vuf_get_features(VirtIODevice *vdev,
                                 uint64_t features,
                                 Error **errp)
{
    VHostUserFS *fs = VHOST_USER_FS(vdev);

    return vhost_get_features(&fs->vhost_dev, user_feature_bits, features);
}

static void vuf_handle_output(VirtIODevice *vdev, VirtQueue *vq)
{
    /*
     * Not normally called; it's the daemon that handles the queue;
     * however virtio's cleanup path can call this.
     */
}

static void vuf_guest_notifier_mask(VirtIODevice *vdev, int idx,
                                            bool mask)
{
    VHostUserFS *fs = VHOST_USER_FS(vdev);

    /*
     * Add the check for configure interrupt, Use VIRTIO_CONFIG_IRQ_IDX -1
     * as the macro of configure interrupt's IDX, If this driver does not
     * support, the function will return
     */

    if (idx == VIRTIO_CONFIG_IRQ_IDX) {
        return;
    }
    vhost_virtqueue_mask(&fs->vhost_dev, vdev, idx, mask);
}

static bool vuf_guest_notifier_pending(VirtIODevice *vdev, int idx)
{
    VHostUserFS *fs = VHOST_USER_FS(vdev);

    /*
     * Add the check for configure interrupt, Use VIRTIO_CONFIG_IRQ_IDX -1
     * as the macro of configure interrupt's IDX, If this driver does not
     * support, the function will return
     */

    if (idx == VIRTIO_CONFIG_IRQ_IDX) {
        return false;
    }
    return vhost_virtqueue_pending(&fs->vhost_dev, idx);
}

static void vuf_device_realize(DeviceState *dev, Error **errp)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    VHostUserFS *fs = VHOST_USER_FS(dev);
    void *cache_ptr;
    unsigned int i;
    size_t len;
    int ret;

    if (!fs->conf.chardev.chr) {
        error_setg(errp, "missing chardev");
        return;
    }

    if (!fs->conf.tag) {
        error_setg(errp, "missing tag property");
        return;
    }
    len = strlen(fs->conf.tag);
    if (len == 0) {
        error_setg(errp, "tag property cannot be empty");
        return;
    }
    if (len > sizeof_field(struct virtio_fs_config, tag)) {
        error_setg(errp, "tag property must be %zu bytes or less",
                   sizeof_field(struct virtio_fs_config, tag));
        return;
    }

    if (fs->conf.num_request_queues == 0) {
        error_setg(errp, "num-request-queues property must be larger than 0");
        return;
    }

    if (!is_power_of_2(fs->conf.queue_size)) {
        error_setg(errp, "queue-size property must be a power of 2");
        return;
    }

    if (fs->conf.queue_size > VIRTQUEUE_MAX_SIZE) {
        error_setg(errp, "queue-size property must be %u or smaller",
                   VIRTQUEUE_MAX_SIZE);
        return;
    }
    if (fs->conf.cache_size &&
        (!is_power_of_2(fs->conf.cache_size) ||
          fs->conf.cache_size < qemu_real_host_page_size())) {
        error_setg(errp, "cache-size property must be a power of 2 "
                         "no smaller than the page size");
        return;
    }
    if (fs->conf.cache_size) {
        /* Anonymous, private memory is not counted as overcommit */
        cache_ptr = mmap(NULL, fs->conf.cache_size, DAX_WINDOW_PROT,
                         MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
        if (cache_ptr == MAP_FAILED) {
            error_setg(errp, "Unable to mmap blank cache");
            return;
        }

        memory_region_init_ram_device_ptr(&fs->cache, OBJECT(vdev),
                                   "virtio-fs-cache",
                                   fs->conf.cache_size, cache_ptr);
    }

    if (!vhost_user_init(&fs->vhost_user, &fs->conf.chardev, errp)) {
        return;
    }

    virtio_init(vdev, VIRTIO_ID_FS, sizeof(struct virtio_fs_config));

    /* Hiprio queue */
    fs->hiprio_vq = virtio_add_queue(vdev, fs->conf.queue_size, vuf_handle_output);

    /* Request queues */
    fs->req_vqs = g_new(VirtQueue *, fs->conf.num_request_queues);
    for (i = 0; i < fs->conf.num_request_queues; i++) {
        fs->req_vqs[i] = virtio_add_queue(vdev, fs->conf.queue_size, vuf_handle_output);
    }

    /* 1 high prio queue, plus the number configured */
    fs->vhost_dev.nvqs = 1 + fs->conf.num_request_queues;
    fs->vhost_dev.vqs = g_new0(struct vhost_virtqueue, fs->vhost_dev.nvqs);
    ret = vhost_dev_init(&fs->vhost_dev, &fs->vhost_user,
                         VHOST_BACKEND_TYPE_USER, 0, errp);
    if (ret < 0) {
        goto err_virtio;
    }

    return;

err_virtio:
    vhost_user_cleanup(&fs->vhost_user);
    virtio_delete_queue(fs->hiprio_vq);
    for (i = 0; i < fs->conf.num_request_queues; i++) {
        virtio_delete_queue(fs->req_vqs[i]);
    }
    g_free(fs->req_vqs);
    virtio_cleanup(vdev);
    g_free(fs->vhost_dev.vqs);
    return;
}

static void vuf_device_unrealize(DeviceState *dev)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    VHostUserFS *fs = VHOST_USER_FS(dev);
    struct vhost_virtqueue *vhost_vqs = fs->vhost_dev.vqs;
    int i;

    /* This will stop vhost backend if appropriate. */
    vuf_set_status(vdev, 0);

    vhost_dev_cleanup(&fs->vhost_dev);

    vhost_user_cleanup(&fs->vhost_user);

    virtio_delete_queue(fs->hiprio_vq);
    for (i = 0; i < fs->conf.num_request_queues; i++) {
        virtio_delete_queue(fs->req_vqs[i]);
    }
    g_free(fs->req_vqs);
    virtio_cleanup(vdev);
    g_free(vhost_vqs);
}

static struct vhost_dev *vuf_get_vhost(VirtIODevice *vdev)
{
    VHostUserFS *fs = VHOST_USER_FS(vdev);
    return &fs->vhost_dev;
}

static const VMStateDescription vuf_vmstate = {
    .name = "vhost-user-fs",
    .unmigratable = 1,
};

static Property vuf_properties[] = {
    DEFINE_PROP_CHR("chardev", VHostUserFS, conf.chardev),
    DEFINE_PROP_STRING("tag", VHostUserFS, conf.tag),
    DEFINE_PROP_UINT16("num-request-queues", VHostUserFS,
                       conf.num_request_queues, 1),
    DEFINE_PROP_UINT16("queue-size", VHostUserFS, conf.queue_size, 128),
    DEFINE_PROP_SIZE("cache-size", VHostUserFS, conf.cache_size, 0),
    DEFINE_PROP_END_OF_LIST(),
};

static void vuf_instance_init(Object *obj)
{
    VHostUserFS *fs = VHOST_USER_FS(obj);

    device_add_bootindex_property(obj, &fs->bootindex, "bootindex",
                                  "/filesystem@0", DEVICE(obj));
}

static void vuf_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioDeviceClass *vdc = VIRTIO_DEVICE_CLASS(klass);

    device_class_set_props(dc, vuf_properties);
    dc->vmsd = &vuf_vmstate;
    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);
    vdc->realize = vuf_device_realize;
    vdc->unrealize = vuf_device_unrealize;
    vdc->get_features = vuf_get_features;
    vdc->get_config = vuf_get_config;
    vdc->set_status = vuf_set_status;
    vdc->guest_notifier_mask = vuf_guest_notifier_mask;
    vdc->guest_notifier_pending = vuf_guest_notifier_pending;
    vdc->get_vhost = vuf_get_vhost;
}

static const TypeInfo vuf_info = {
    .name = TYPE_VHOST_USER_FS,
    .parent = TYPE_VIRTIO_DEVICE,
    .instance_size = sizeof(VHostUserFS),
    .instance_init = vuf_instance_init,
    .class_init = vuf_class_init,
};

static void vuf_register_types(void)
{
    type_register_static(&vuf_info);
}

type_init(vuf_register_types)
