/*
 * Virtio GPU Device
 *
 * Copyright Red Hat, Inc. 2013-2014
 *
 * Authors:
 *     Dave Airlie <airlied@redhat.com>
 *     Gerd Hoffmann <kraxel@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/iov.h"
#include "ui/console.h"
#include "trace.h"
#include "sysemu/dma.h"
#include "sysemu/sysemu.h"
#include "hw/virtio/virtio.h"
#include "migration/qemu-file-types.h"
#include "hw/virtio/virtio-gpu.h"
#include "hw/virtio/virtio-gpu-bswap.h"
#include "hw/virtio/virtio-gpu-pixman.h"
#include "hw/virtio/virtio-bus.h"
#include "hw/display/edid.h"
#include "hw/qdev-properties.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "qemu/error-report.h"

#define VIRTIO_GPU_VM_VERSION 1

static struct virtio_gpu_simple_resource*
virtio_gpu_find_resource(VirtIOGPU *g, uint32_t resource_id);

static void virtio_gpu_cleanup_mapping(VirtIOGPU *g,
                                       struct virtio_gpu_simple_resource *res);

#ifdef CONFIG_VIRGL
#include <virglrenderer.h>
#define VIRGL(_g, _virgl, _simple, ...)                     \
    do {                                                    \
        if (_g->parent_obj.use_virgl_renderer) {            \
            _virgl(__VA_ARGS__);                            \
        } else {                                            \
            _simple(__VA_ARGS__);                           \
        }                                                   \
    } while (0)
#else
#define VIRGL(_g, _virgl, _simple, ...)                 \
    do {                                                \
        _simple(__VA_ARGS__);                           \
    } while (0)
#endif

static void update_cursor_data_simple(VirtIOGPU *g,
                                      struct virtio_gpu_scanout *s,
                                      uint32_t resource_id)
{
    struct virtio_gpu_simple_resource *res;
    uint32_t pixels;

    res = virtio_gpu_find_resource(g, resource_id);
    if (!res) {
        return;
    }

    if (pixman_image_get_width(res->image)  != s->current_cursor->width ||
        pixman_image_get_height(res->image) != s->current_cursor->height) {
        return;
    }

    pixels = s->current_cursor->width * s->current_cursor->height;
    memcpy(s->current_cursor->data,
           pixman_image_get_data(res->image),
           pixels * sizeof(uint32_t));
}

#ifdef CONFIG_VIRGL

static void update_cursor_data_virgl(VirtIOGPU *g,
                                     struct virtio_gpu_scanout *s,
                                     uint32_t resource_id)
{
    uint32_t width, height;
    uint32_t pixels, *data;

    data = virgl_renderer_get_cursor_data(resource_id, &width, &height);
    if (!data) {
        return;
    }

    if (width != s->current_cursor->width ||
        height != s->current_cursor->height) {
        free(data);
        return;
    }

    pixels = s->current_cursor->width * s->current_cursor->height;
    memcpy(s->current_cursor->data, data, pixels * sizeof(uint32_t));
    free(data);
}

#endif

static void update_cursor(VirtIOGPU *g, struct virtio_gpu_update_cursor *cursor)
{
    struct virtio_gpu_scanout *s;
    bool move = cursor->hdr.type == VIRTIO_GPU_CMD_MOVE_CURSOR;

    if (cursor->pos.scanout_id >= g->parent_obj.conf.max_outputs) {
        return;
    }
    s = &g->parent_obj.scanout[cursor->pos.scanout_id];

    trace_virtio_gpu_update_cursor(cursor->pos.scanout_id,
                                   cursor->pos.x,
                                   cursor->pos.y,
                                   move ? "move" : "update",
                                   cursor->resource_id);

    if (!move) {
        if (!s->current_cursor) {
            s->current_cursor = cursor_alloc(64, 64);
        }

        s->current_cursor->hot_x = cursor->hot_x;
        s->current_cursor->hot_y = cursor->hot_y;

        if (cursor->resource_id > 0) {
            VIRGL(g, update_cursor_data_virgl, update_cursor_data_simple,
                  g, s, cursor->resource_id);
        }
        dpy_cursor_define(s->con, s->current_cursor);

        s->cursor = *cursor;
    } else {
        s->cursor.pos.x = cursor->pos.x;
        s->cursor.pos.y = cursor->pos.y;
    }
    dpy_mouse_set(s->con, cursor->pos.x, cursor->pos.y,
                  cursor->resource_id ? 1 : 0);
}

static struct virtio_gpu_simple_resource *
virtio_gpu_find_resource(VirtIOGPU *g, uint32_t resource_id)
{
    struct virtio_gpu_simple_resource *res;

    QTAILQ_FOREACH(res, &g->reslist, next) {
        if (res->resource_id == resource_id) {
            return res;
        }
    }
    return NULL;
}

void virtio_gpu_ctrl_response(VirtIOGPU *g,
                              struct virtio_gpu_ctrl_command *cmd,
                              struct virtio_gpu_ctrl_hdr *resp,
                              size_t resp_len)
{
    size_t s;

    if (cmd->cmd_hdr.flags & VIRTIO_GPU_FLAG_FENCE) {
        resp->flags |= VIRTIO_GPU_FLAG_FENCE;
        resp->fence_id = cmd->cmd_hdr.fence_id;
        resp->ctx_id = cmd->cmd_hdr.ctx_id;
    }
    virtio_gpu_ctrl_hdr_bswap(resp);
    s = iov_from_buf(cmd->elem.in_sg, cmd->elem.in_num, 0, resp, resp_len);
    if (s != resp_len) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: response size incorrect %zu vs %zu\n",
                      __func__, s, resp_len);
    }
    virtqueue_push(cmd->vq, &cmd->elem, s);
    virtio_notify(VIRTIO_DEVICE(g), cmd->vq);
    cmd->finished = true;
}

void virtio_gpu_ctrl_response_nodata(VirtIOGPU *g,
                                     struct virtio_gpu_ctrl_command *cmd,
                                     enum virtio_gpu_ctrl_type type)
{
    struct virtio_gpu_ctrl_hdr resp;

    memset(&resp, 0, sizeof(resp));
    resp.type = type;
    virtio_gpu_ctrl_response(g, cmd, &resp, sizeof(resp));
}

void virtio_gpu_get_display_info(VirtIOGPU *g,
                                 struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_resp_display_info display_info;

    trace_virtio_gpu_cmd_get_display_info();
    memset(&display_info, 0, sizeof(display_info));
    display_info.hdr.type = VIRTIO_GPU_RESP_OK_DISPLAY_INFO;
    virtio_gpu_base_fill_display_info(VIRTIO_GPU_BASE(g), &display_info);
    virtio_gpu_ctrl_response(g, cmd, &display_info.hdr,
                             sizeof(display_info));
}

static void
virtio_gpu_generate_edid(VirtIOGPU *g, int scanout,
                         struct virtio_gpu_resp_edid *edid)
{
    VirtIOGPUBase *b = VIRTIO_GPU_BASE(g);
    qemu_edid_info info = {
        .width_mm = b->req_state[scanout].width_mm,
        .height_mm = b->req_state[scanout].height_mm,
        .prefx = b->req_state[scanout].width,
        .prefy = b->req_state[scanout].height,
    };

    edid->size = cpu_to_le32(sizeof(edid->edid));
    qemu_edid_generate(edid->edid, sizeof(edid->edid), &info);
}

void virtio_gpu_get_edid(VirtIOGPU *g,
                         struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_resp_edid edid;
    struct virtio_gpu_cmd_get_edid get_edid;
    VirtIOGPUBase *b = VIRTIO_GPU_BASE(g);

    VIRTIO_GPU_FILL_CMD(get_edid);
    virtio_gpu_bswap_32(&get_edid, sizeof(get_edid));

    if (get_edid.scanout >= b->conf.max_outputs) {
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
        return;
    }

    trace_virtio_gpu_cmd_get_edid(get_edid.scanout);
    memset(&edid, 0, sizeof(edid));
    edid.hdr.type = VIRTIO_GPU_RESP_OK_EDID;
    virtio_gpu_generate_edid(g, get_edid.scanout, &edid);
    virtio_gpu_ctrl_response(g, cmd, &edid.hdr, sizeof(edid));
}

static uint32_t calc_image_hostmem(pixman_format_code_t pformat,
                                   uint32_t width, uint32_t height)
{
    /* Copied from pixman/pixman-bits-image.c, skip integer overflow check.
     * pixman_image_create_bits will fail in case it overflow.
     */

    int bpp = PIXMAN_FORMAT_BPP(pformat);
    int stride = ((width * bpp + 0x1f) >> 5) * sizeof(uint32_t);
    return height * stride;
}

static void virtio_gpu_resource_create_2d(VirtIOGPU *g,
                                          struct virtio_gpu_ctrl_command *cmd)
{
    pixman_format_code_t pformat;
    struct virtio_gpu_simple_resource *res;
    struct virtio_gpu_resource_create_2d c2d;

    VIRTIO_GPU_FILL_CMD(c2d);
    virtio_gpu_bswap_32(&c2d, sizeof(c2d));
    trace_virtio_gpu_cmd_res_create_2d(c2d.resource_id, c2d.format,
                                       c2d.width, c2d.height);

    if (c2d.resource_id == 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: resource id 0 is not allowed\n",
                      __func__);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
        return;
    }

    res = virtio_gpu_find_resource(g, c2d.resource_id);
    if (res) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: resource already exists %d\n",
                      __func__, c2d.resource_id);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
        return;
    }

    res = g_new0(struct virtio_gpu_simple_resource, 1);

    res->width = c2d.width;
    res->height = c2d.height;
    res->format = c2d.format;
    res->resource_id = c2d.resource_id;

    pformat = virtio_gpu_get_pixman_format(c2d.format);
    if (!pformat) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: host couldn't handle guest format %d\n",
                      __func__, c2d.format);
        g_free(res);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
        return;
    }

    res->hostmem = calc_image_hostmem(pformat, c2d.width, c2d.height);
    if (res->hostmem + g->hostmem < g->conf_max_hostmem) {
        res->image = pixman_image_create_bits(pformat,
                                              c2d.width,
                                              c2d.height,
                                              NULL, 0);
    }

    if (!res->image) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: resource creation failed %d %d %d\n",
                      __func__, c2d.resource_id, c2d.width, c2d.height);
        g_free(res);
        cmd->error = VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY;
        return;
    }

    QTAILQ_INSERT_HEAD(&g->reslist, res, next);
    g->hostmem += res->hostmem;
}

static void virtio_gpu_disable_scanout(VirtIOGPU *g, int scanout_id)
{
    struct virtio_gpu_scanout *scanout = &g->parent_obj.scanout[scanout_id];
    struct virtio_gpu_simple_resource *res;

    if (scanout->resource_id == 0) {
        return;
    }

    res = virtio_gpu_find_resource(g, scanout->resource_id);
    if (res) {
        res->scanout_bitmask &= ~(1 << scanout_id);
    }

    dpy_gfx_replace_surface(scanout->con, NULL);
    scanout->resource_id = 0;
    scanout->ds = NULL;
    scanout->width = 0;
    scanout->height = 0;
}

static void virtio_gpu_resource_destroy(VirtIOGPU *g,
                                        struct virtio_gpu_simple_resource *res)
{
    int i;

    if (res->scanout_bitmask) {
        for (i = 0; i < g->parent_obj.conf.max_outputs; i++) {
            if (res->scanout_bitmask & (1 << i)) {
                virtio_gpu_disable_scanout(g, i);
            }
        }
    }

    pixman_image_unref(res->image);
    virtio_gpu_cleanup_mapping(g, res);
    QTAILQ_REMOVE(&g->reslist, res, next);
    g->hostmem -= res->hostmem;
    g_free(res);
}

static void virtio_gpu_resource_unref(VirtIOGPU *g,
                                      struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_simple_resource *res;
    struct virtio_gpu_resource_unref unref;

    VIRTIO_GPU_FILL_CMD(unref);
    virtio_gpu_bswap_32(&unref, sizeof(unref));
    trace_virtio_gpu_cmd_res_unref(unref.resource_id);

    res = virtio_gpu_find_resource(g, unref.resource_id);
    if (!res) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: illegal resource specified %d\n",
                      __func__, unref.resource_id);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
        return;
    }
    virtio_gpu_resource_destroy(g, res);
}

static void virtio_gpu_transfer_to_host_2d(VirtIOGPU *g,
                                           struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_simple_resource *res;
    int h;
    uint32_t src_offset, dst_offset, stride;
    int bpp;
    pixman_format_code_t format;
    struct virtio_gpu_transfer_to_host_2d t2d;

    VIRTIO_GPU_FILL_CMD(t2d);
    virtio_gpu_t2d_bswap(&t2d);
    trace_virtio_gpu_cmd_res_xfer_toh_2d(t2d.resource_id);

    res = virtio_gpu_find_resource(g, t2d.resource_id);
    if (!res || !res->iov) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: illegal resource specified %d\n",
                      __func__, t2d.resource_id);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
        return;
    }

    if (t2d.r.x > res->width ||
        t2d.r.y > res->height ||
        t2d.r.width > res->width ||
        t2d.r.height > res->height ||
        t2d.r.x + t2d.r.width > res->width ||
        t2d.r.y + t2d.r.height > res->height) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: transfer bounds outside resource"
                      " bounds for resource %d: %d %d %d %d vs %d %d\n",
                      __func__, t2d.resource_id, t2d.r.x, t2d.r.y,
                      t2d.r.width, t2d.r.height, res->width, res->height);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
        return;
    }

    format = pixman_image_get_format(res->image);
    bpp = DIV_ROUND_UP(PIXMAN_FORMAT_BPP(format), 8);
    stride = pixman_image_get_stride(res->image);

    if (t2d.offset || t2d.r.x || t2d.r.y ||
        t2d.r.width != pixman_image_get_width(res->image)) {
        void *img_data = pixman_image_get_data(res->image);
        for (h = 0; h < t2d.r.height; h++) {
            src_offset = t2d.offset + stride * h;
            dst_offset = (t2d.r.y + h) * stride + (t2d.r.x * bpp);

            iov_to_buf(res->iov, res->iov_cnt, src_offset,
                       (uint8_t *)img_data
                       + dst_offset, t2d.r.width * bpp);
        }
    } else {
        iov_to_buf(res->iov, res->iov_cnt, 0,
                   pixman_image_get_data(res->image),
                   pixman_image_get_stride(res->image)
                   * pixman_image_get_height(res->image));
    }
}

static void virtio_gpu_resource_flush(VirtIOGPU *g,
                                      struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_simple_resource *res;
    struct virtio_gpu_resource_flush rf;
    pixman_region16_t flush_region;
    int i;

    VIRTIO_GPU_FILL_CMD(rf);
    virtio_gpu_bswap_32(&rf, sizeof(rf));
    trace_virtio_gpu_cmd_res_flush(rf.resource_id,
                                   rf.r.width, rf.r.height, rf.r.x, rf.r.y);

    res = virtio_gpu_find_resource(g, rf.resource_id);
    if (!res) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: illegal resource specified %d\n",
                      __func__, rf.resource_id);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
        return;
    }

    if (rf.r.x > res->width ||
        rf.r.y > res->height ||
        rf.r.width > res->width ||
        rf.r.height > res->height ||
        rf.r.x + rf.r.width > res->width ||
        rf.r.y + rf.r.height > res->height) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: flush bounds outside resource"
                      " bounds for resource %d: %d %d %d %d vs %d %d\n",
                      __func__, rf.resource_id, rf.r.x, rf.r.y,
                      rf.r.width, rf.r.height, res->width, res->height);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
        return;
    }

    pixman_region_init_rect(&flush_region,
                            rf.r.x, rf.r.y, rf.r.width, rf.r.height);
    for (i = 0; i < g->parent_obj.conf.max_outputs; i++) {
        struct virtio_gpu_scanout *scanout;
        pixman_region16_t region, finalregion;
        pixman_box16_t *extents;

        if (!(res->scanout_bitmask & (1 << i))) {
            continue;
        }
        scanout = &g->parent_obj.scanout[i];

        pixman_region_init(&finalregion);
        pixman_region_init_rect(&region, scanout->x, scanout->y,
                                scanout->width, scanout->height);

        pixman_region_intersect(&finalregion, &flush_region, &region);
        pixman_region_translate(&finalregion, -scanout->x, -scanout->y);
        extents = pixman_region_extents(&finalregion);
        /* work out the area we need to update for each console */
        dpy_gfx_update(g->parent_obj.scanout[i].con,
                       extents->x1, extents->y1,
                       extents->x2 - extents->x1,
                       extents->y2 - extents->y1);

        pixman_region_fini(&region);
        pixman_region_fini(&finalregion);
    }
    pixman_region_fini(&flush_region);
}

static void virtio_unref_resource(pixman_image_t *image, void *data)
{
    pixman_image_unref(data);
}

static void virtio_gpu_set_scanout(VirtIOGPU *g,
                                   struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_simple_resource *res, *ores;
    struct virtio_gpu_scanout *scanout;
    pixman_format_code_t format;
    uint32_t offset;
    int bpp;
    struct virtio_gpu_set_scanout ss;

    VIRTIO_GPU_FILL_CMD(ss);
    virtio_gpu_bswap_32(&ss, sizeof(ss));
    trace_virtio_gpu_cmd_set_scanout(ss.scanout_id, ss.resource_id,
                                     ss.r.width, ss.r.height, ss.r.x, ss.r.y);

    if (ss.scanout_id >= g->parent_obj.conf.max_outputs) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: illegal scanout id specified %d",
                      __func__, ss.scanout_id);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID;
        return;
    }

    g->parent_obj.enable = 1;
    if (ss.resource_id == 0) {
        virtio_gpu_disable_scanout(g, ss.scanout_id);
        return;
    }

    /* create a surface for this scanout */
    res = virtio_gpu_find_resource(g, ss.resource_id);
    if (!res) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: illegal resource specified %d\n",
                      __func__, ss.resource_id);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
        return;
    }

    if (ss.r.x > res->width ||
        ss.r.y > res->height ||
        ss.r.width < 16 ||
        ss.r.height < 16 ||
        ss.r.width > res->width ||
        ss.r.height > res->height ||
        ss.r.x + ss.r.width > res->width ||
        ss.r.y + ss.r.height > res->height) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: illegal scanout %d bounds for"
                      " resource %d, (%d,%d)+%d,%d vs %d %d\n",
                      __func__, ss.scanout_id, ss.resource_id, ss.r.x, ss.r.y,
                      ss.r.width, ss.r.height, res->width, res->height);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
        return;
    }

    scanout = &g->parent_obj.scanout[ss.scanout_id];

    format = pixman_image_get_format(res->image);
    bpp = DIV_ROUND_UP(PIXMAN_FORMAT_BPP(format), 8);
    offset = (ss.r.x * bpp) + ss.r.y * pixman_image_get_stride(res->image);
    if (!scanout->ds || surface_data(scanout->ds)
        != ((uint8_t *)pixman_image_get_data(res->image) + offset) ||
        scanout->width != ss.r.width ||
        scanout->height != ss.r.height) {
        pixman_image_t *rect;
        void *ptr = (uint8_t *)pixman_image_get_data(res->image) + offset;
        rect = pixman_image_create_bits(format, ss.r.width, ss.r.height, ptr,
                                        pixman_image_get_stride(res->image));
        pixman_image_ref(res->image);
        pixman_image_set_destroy_function(rect, virtio_unref_resource,
                                          res->image);
        /* realloc the surface ptr */
        scanout->ds = qemu_create_displaysurface_pixman(rect);
        if (!scanout->ds) {
            cmd->error = VIRTIO_GPU_RESP_ERR_UNSPEC;
            return;
        }
        pixman_image_unref(rect);
        dpy_gfx_replace_surface(g->parent_obj.scanout[ss.scanout_id].con,
                                scanout->ds);
    }

    ores = virtio_gpu_find_resource(g, scanout->resource_id);
    if (ores) {
        ores->scanout_bitmask &= ~(1 << ss.scanout_id);
    }

    res->scanout_bitmask |= (1 << ss.scanout_id);
    scanout->resource_id = ss.resource_id;
    scanout->x = ss.r.x;
    scanout->y = ss.r.y;
    scanout->width = ss.r.width;
    scanout->height = ss.r.height;
}

int virtio_gpu_create_mapping_iov(VirtIOGPU *g,
                                  struct virtio_gpu_resource_attach_backing *ab,
                                  struct virtio_gpu_ctrl_command *cmd,
                                  uint64_t **addr, struct iovec **iov,
                                  uint32_t *niov)
{
    struct virtio_gpu_mem_entry *ents;
    size_t esize, s;
    int e, v;

    if (ab->nr_entries > 16384) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: nr_entries is too big (%d > 16384)\n",
                      __func__, ab->nr_entries);
        return -1;
    }

    esize = sizeof(*ents) * ab->nr_entries;
    ents = g_malloc(esize);
    s = iov_to_buf(cmd->elem.out_sg, cmd->elem.out_num,
                   sizeof(*ab), ents, esize);
    if (s != esize) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: command data size incorrect %zu vs %zu\n",
                      __func__, s, esize);
        g_free(ents);
        return -1;
    }

    *iov = NULL;
    if (addr) {
        *addr = NULL;
    }
    for (e = 0, v = 0; e < ab->nr_entries; e++) {
        uint64_t a = le64_to_cpu(ents[e].addr);
        uint32_t l = le32_to_cpu(ents[e].length);
        hwaddr len;
        void *map;

        do {
            len = l;
            map = dma_memory_map(VIRTIO_DEVICE(g)->dma_as,
                                 a, &len, DMA_DIRECTION_TO_DEVICE);
            if (!map) {
                qemu_log_mask(LOG_GUEST_ERROR, "%s: failed to map MMIO memory for"
                              " resource %d element %d\n",
                              __func__, ab->resource_id, e);
                virtio_gpu_cleanup_mapping_iov(g, *iov, v);
                g_free(ents);
                *iov = NULL;
                if (addr) {
                    g_free(*addr);
                    *addr = NULL;
                }
                return -1;
            }

            if (!(v % 16)) {
                *iov = g_realloc(*iov, sizeof(struct iovec) * (v + 16));
                if (addr) {
                    *addr = g_realloc(*addr, sizeof(uint64_t) * (v + 16));
                }
            }
            (*iov)[v].iov_base = map;
            (*iov)[v].iov_len = len;
            if (addr) {
                (*addr)[v] = a;
            }

            a += len;
            l -= len;
            v += 1;
        } while (l > 0);
    }
    *niov = v;

    g_free(ents);
    return 0;
}

void virtio_gpu_cleanup_mapping_iov(VirtIOGPU *g,
                                    struct iovec *iov, uint32_t count)
{
    int i;

    for (i = 0; i < count; i++) {
        dma_memory_unmap(VIRTIO_DEVICE(g)->dma_as,
                         iov[i].iov_base, iov[i].iov_len,
                         DMA_DIRECTION_TO_DEVICE,
                         iov[i].iov_len);
    }
    g_free(iov);
}

static void virtio_gpu_cleanup_mapping(VirtIOGPU *g,
                                       struct virtio_gpu_simple_resource *res)
{
    virtio_gpu_cleanup_mapping_iov(g, res->iov, res->iov_cnt);
    res->iov = NULL;
    res->iov_cnt = 0;
    g_free(res->addrs);
    res->addrs = NULL;
}

static void
virtio_gpu_resource_attach_backing(VirtIOGPU *g,
                                   struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_simple_resource *res;
    struct virtio_gpu_resource_attach_backing ab;
    int ret;

    VIRTIO_GPU_FILL_CMD(ab);
    virtio_gpu_bswap_32(&ab, sizeof(ab));
    trace_virtio_gpu_cmd_res_back_attach(ab.resource_id);

    res = virtio_gpu_find_resource(g, ab.resource_id);
    if (!res) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: illegal resource specified %d\n",
                      __func__, ab.resource_id);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
        return;
    }

    if (res->iov) {
        cmd->error = VIRTIO_GPU_RESP_ERR_UNSPEC;
        return;
    }

    ret = virtio_gpu_create_mapping_iov(g, &ab, cmd, &res->addrs,
                                        &res->iov, &res->iov_cnt);
    if (ret != 0) {
        cmd->error = VIRTIO_GPU_RESP_ERR_UNSPEC;
        return;
    }
}

static void
virtio_gpu_resource_detach_backing(VirtIOGPU *g,
                                   struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_simple_resource *res;
    struct virtio_gpu_resource_detach_backing detach;

    VIRTIO_GPU_FILL_CMD(detach);
    virtio_gpu_bswap_32(&detach, sizeof(detach));
    trace_virtio_gpu_cmd_res_back_detach(detach.resource_id);

    res = virtio_gpu_find_resource(g, detach.resource_id);
    if (!res || !res->iov) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: illegal resource specified %d\n",
                      __func__, detach.resource_id);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
        return;
    }
    virtio_gpu_cleanup_mapping(g, res);
}

static void virtio_gpu_simple_process_cmd(VirtIOGPU *g,
                                          struct virtio_gpu_ctrl_command *cmd)
{
    VIRTIO_GPU_FILL_CMD(cmd->cmd_hdr);
    virtio_gpu_ctrl_hdr_bswap(&cmd->cmd_hdr);

    switch (cmd->cmd_hdr.type) {
    case VIRTIO_GPU_CMD_GET_DISPLAY_INFO:
        virtio_gpu_get_display_info(g, cmd);
        break;
    case VIRTIO_GPU_CMD_GET_EDID:
        virtio_gpu_get_edid(g, cmd);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_CREATE_2D:
        virtio_gpu_resource_create_2d(g, cmd);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_UNREF:
        virtio_gpu_resource_unref(g, cmd);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_FLUSH:
        virtio_gpu_resource_flush(g, cmd);
        break;
    case VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D:
        virtio_gpu_transfer_to_host_2d(g, cmd);
        break;
    case VIRTIO_GPU_CMD_SET_SCANOUT:
        virtio_gpu_set_scanout(g, cmd);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING:
        virtio_gpu_resource_attach_backing(g, cmd);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING:
        virtio_gpu_resource_detach_backing(g, cmd);
        break;
    default:
        cmd->error = VIRTIO_GPU_RESP_ERR_UNSPEC;
        break;
    }
    if (!cmd->finished) {
        virtio_gpu_ctrl_response_nodata(g, cmd, cmd->error ? cmd->error :
                                        VIRTIO_GPU_RESP_OK_NODATA);
    }
}

static void virtio_gpu_handle_ctrl_cb(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtIOGPU *g = VIRTIO_GPU(vdev);
    qemu_bh_schedule(g->ctrl_bh);
}

static void virtio_gpu_handle_cursor_cb(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtIOGPU *g = VIRTIO_GPU(vdev);
    qemu_bh_schedule(g->cursor_bh);
}

void virtio_gpu_process_cmdq(VirtIOGPU *g)
{
    struct virtio_gpu_ctrl_command *cmd;

    if (g->processing_cmdq) {
        return;
    }
    g->processing_cmdq = true;
    while (!QTAILQ_EMPTY(&g->cmdq)) {
        cmd = QTAILQ_FIRST(&g->cmdq);

        if (g->parent_obj.renderer_blocked) {
            break;
        }

        /* process command */
        VIRGL(g, virtio_gpu_virgl_process_cmd, virtio_gpu_simple_process_cmd,
              g, cmd);

        QTAILQ_REMOVE(&g->cmdq, cmd, next);
        if (virtio_gpu_stats_enabled(g->parent_obj.conf)) {
            g->stats.requests++;
        }

        if (!cmd->finished) {
            QTAILQ_INSERT_TAIL(&g->fenceq, cmd, next);
            g->inflight++;
            if (virtio_gpu_stats_enabled(g->parent_obj.conf)) {
                if (g->stats.max_inflight < g->inflight) {
                    g->stats.max_inflight = g->inflight;
                }
                fprintf(stderr, "inflight: %3d (+)\r", g->inflight);
            }
        } else {
            g_free(cmd);
        }
    }
    g->processing_cmdq = false;
}

static void virtio_gpu_gl_flushed(VirtIOGPUBase *b)
{
    VirtIOGPU *g = VIRTIO_GPU(b);

#ifdef CONFIG_VIRGL
    if (g->renderer_reset) {
        g->renderer_reset = false;
        virtio_gpu_virgl_reset(g);
    }
#endif
    virtio_gpu_process_cmdq(g);
}

static void virtio_gpu_handle_ctrl(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtIOGPU *g = VIRTIO_GPU(vdev);
    struct virtio_gpu_ctrl_command *cmd;

    if (!virtio_queue_ready(vq)) {
        return;
    }

#ifdef CONFIG_VIRGL
    if (!g->renderer_inited && g->parent_obj.use_virgl_renderer) {
        virtio_gpu_virgl_init(g);
        g->renderer_inited = true;
    }
#endif

    cmd = virtqueue_pop(vq, sizeof(struct virtio_gpu_ctrl_command));
    while (cmd) {
        cmd->vq = vq;
        cmd->error = 0;
        cmd->finished = false;
        QTAILQ_INSERT_TAIL(&g->cmdq, cmd, next);
        cmd = virtqueue_pop(vq, sizeof(struct virtio_gpu_ctrl_command));
    }

    virtio_gpu_process_cmdq(g);

#ifdef CONFIG_VIRGL
    if (g->parent_obj.use_virgl_renderer) {
        virtio_gpu_virgl_fence_poll(g);
    }
#endif
}

static void virtio_gpu_ctrl_bh(void *opaque)
{
    VirtIOGPU *g = opaque;
    virtio_gpu_handle_ctrl(&g->parent_obj.parent_obj, g->ctrl_vq);
}

static void virtio_gpu_handle_cursor(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtIOGPU *g = VIRTIO_GPU(vdev);
    VirtQueueElement *elem;
    size_t s;
    struct virtio_gpu_update_cursor cursor_info;

    if (!virtio_queue_ready(vq)) {
        return;
    }
    for (;;) {
        elem = virtqueue_pop(vq, sizeof(VirtQueueElement));
        if (!elem) {
            break;
        }

        s = iov_to_buf(elem->out_sg, elem->out_num, 0,
                       &cursor_info, sizeof(cursor_info));
        if (s != sizeof(cursor_info)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: cursor size incorrect %zu vs %zu\n",
                          __func__, s, sizeof(cursor_info));
        } else {
            virtio_gpu_bswap_32(&cursor_info, sizeof(cursor_info));
            update_cursor(g, &cursor_info);
        }
        virtqueue_push(vq, elem, 0);
        virtio_notify(vdev, vq);
        g_free(elem);
    }
}

static void virtio_gpu_cursor_bh(void *opaque)
{
    VirtIOGPU *g = opaque;
    virtio_gpu_handle_cursor(&g->parent_obj.parent_obj, g->cursor_vq);
}

static const VMStateDescription vmstate_virtio_gpu_scanout = {
    .name = "virtio-gpu-one-scanout",
    .version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(resource_id, struct virtio_gpu_scanout),
        VMSTATE_UINT32(width, struct virtio_gpu_scanout),
        VMSTATE_UINT32(height, struct virtio_gpu_scanout),
        VMSTATE_INT32(x, struct virtio_gpu_scanout),
        VMSTATE_INT32(y, struct virtio_gpu_scanout),
        VMSTATE_UINT32(cursor.resource_id, struct virtio_gpu_scanout),
        VMSTATE_UINT32(cursor.hot_x, struct virtio_gpu_scanout),
        VMSTATE_UINT32(cursor.hot_y, struct virtio_gpu_scanout),
        VMSTATE_UINT32(cursor.pos.x, struct virtio_gpu_scanout),
        VMSTATE_UINT32(cursor.pos.y, struct virtio_gpu_scanout),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_virtio_gpu_scanouts = {
    .name = "virtio-gpu-scanouts",
    .version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_INT32(parent_obj.enable, struct VirtIOGPU),
        VMSTATE_UINT32_EQUAL(parent_obj.conf.max_outputs,
                             struct VirtIOGPU, NULL),
        VMSTATE_STRUCT_VARRAY_UINT32(parent_obj.scanout, struct VirtIOGPU,
                                     parent_obj.conf.max_outputs, 1,
                                     vmstate_virtio_gpu_scanout,
                                     struct virtio_gpu_scanout),
        VMSTATE_END_OF_LIST()
    },
};

static int virtio_gpu_save(QEMUFile *f, void *opaque, size_t size,
                           const VMStateField *field, JSONWriter *vmdesc)
{
    VirtIOGPU *g = opaque;
    struct virtio_gpu_simple_resource *res;
    int i;

    /* in 2d mode we should never find unprocessed commands here */
    assert(QTAILQ_EMPTY(&g->cmdq));

    QTAILQ_FOREACH(res, &g->reslist, next) {
        qemu_put_be32(f, res->resource_id);
        qemu_put_be32(f, res->width);
        qemu_put_be32(f, res->height);
        qemu_put_be32(f, res->format);
        qemu_put_be32(f, res->iov_cnt);
        for (i = 0; i < res->iov_cnt; i++) {
            qemu_put_be64(f, res->addrs[i]);
            qemu_put_be32(f, res->iov[i].iov_len);
        }
        qemu_put_buffer(f, (void *)pixman_image_get_data(res->image),
                        pixman_image_get_stride(res->image) * res->height);
    }
    qemu_put_be32(f, 0); /* end of list */

    return vmstate_save_state(f, &vmstate_virtio_gpu_scanouts, g, NULL);
}

static int virtio_gpu_load(QEMUFile *f, void *opaque, size_t size,
                           const VMStateField *field)
{
    VirtIOGPU *g = opaque;
    struct virtio_gpu_simple_resource *res;
    struct virtio_gpu_scanout *scanout;
    uint32_t resource_id, pformat;
    int i;

    g->hostmem = 0;

    resource_id = qemu_get_be32(f);
    while (resource_id != 0) {
        res = virtio_gpu_find_resource(g, resource_id);
        if (res) {
            return -EINVAL;
        }

        res = g_new0(struct virtio_gpu_simple_resource, 1);
        res->resource_id = resource_id;
        res->width = qemu_get_be32(f);
        res->height = qemu_get_be32(f);
        res->format = qemu_get_be32(f);
        res->iov_cnt = qemu_get_be32(f);

        /* allocate */
        pformat = virtio_gpu_get_pixman_format(res->format);
        if (!pformat) {
            g_free(res);
            return -EINVAL;
        }
        res->image = pixman_image_create_bits(pformat,
                                              res->width, res->height,
                                              NULL, 0);
        if (!res->image) {
            g_free(res);
            return -EINVAL;
        }

        res->hostmem = calc_image_hostmem(pformat, res->width, res->height);

        res->addrs = g_new(uint64_t, res->iov_cnt);
        res->iov = g_new(struct iovec, res->iov_cnt);

        /* read data */
        for (i = 0; i < res->iov_cnt; i++) {
            res->addrs[i] = qemu_get_be64(f);
            res->iov[i].iov_len = qemu_get_be32(f);
        }
        qemu_get_buffer(f, (void *)pixman_image_get_data(res->image),
                        pixman_image_get_stride(res->image) * res->height);

        /* restore mapping */
        for (i = 0; i < res->iov_cnt; i++) {
            hwaddr len = res->iov[i].iov_len;
            res->iov[i].iov_base =
                dma_memory_map(VIRTIO_DEVICE(g)->dma_as,
                               res->addrs[i], &len, DMA_DIRECTION_TO_DEVICE);

            if (!res->iov[i].iov_base || len != res->iov[i].iov_len) {
                /* Clean up the half-a-mapping we just created... */
                if (res->iov[i].iov_base) {
                    dma_memory_unmap(VIRTIO_DEVICE(g)->dma_as,
                                     res->iov[i].iov_base,
                                     len,
                                     DMA_DIRECTION_TO_DEVICE,
                                     0);
                }
                /* ...and the mappings for previous loop iterations */
                res->iov_cnt = i;
                virtio_gpu_cleanup_mapping(g, res);
                pixman_image_unref(res->image);
                g_free(res);
                return -EINVAL;
            }
        }

        QTAILQ_INSERT_HEAD(&g->reslist, res, next);
        g->hostmem += res->hostmem;

        resource_id = qemu_get_be32(f);
    }

    /* load & apply scanout state */
    vmstate_load_state(f, &vmstate_virtio_gpu_scanouts, g, 1);
    for (i = 0; i < g->parent_obj.conf.max_outputs; i++) {
        scanout = &g->parent_obj.scanout[i];
        if (!scanout->resource_id) {
            continue;
        }
        res = virtio_gpu_find_resource(g, scanout->resource_id);
        if (!res) {
            return -EINVAL;
        }
        scanout->ds = qemu_create_displaysurface_pixman(res->image);
        if (!scanout->ds) {
            return -EINVAL;
        }

        dpy_gfx_replace_surface(scanout->con, scanout->ds);
        dpy_gfx_update_full(scanout->con);
        if (scanout->cursor.resource_id) {
            update_cursor(g, &scanout->cursor);
        }
        res->scanout_bitmask |= (1 << i);
    }

    return 0;
}

static void virtio_gpu_device_realize(DeviceState *qdev, Error **errp)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(qdev);
    VirtIOGPU *g = VIRTIO_GPU(qdev);
    bool have_virgl;

#if !defined(CONFIG_VIRGL) || defined(HOST_WORDS_BIGENDIAN)
    have_virgl = false;
#else
    have_virgl = display_opengl;
#endif
    if (!have_virgl) {
        g->parent_obj.conf.flags &= ~(1 << VIRTIO_GPU_FLAG_VIRGL_ENABLED);
    } else {
#if defined(CONFIG_VIRGL)
        VIRTIO_GPU_BASE(g)->virtio_config.num_capsets =
            virtio_gpu_virgl_get_num_capsets(g);
#endif
    }

    if (!virtio_gpu_base_device_realize(qdev,
                                        virtio_gpu_handle_ctrl_cb,
                                        virtio_gpu_handle_cursor_cb,
                                        errp)) {
        return;
    }

    g->ctrl_vq = virtio_get_queue(vdev, 0);
    g->cursor_vq = virtio_get_queue(vdev, 1);
    g->ctrl_bh = qemu_bh_new(virtio_gpu_ctrl_bh, g);
    g->cursor_bh = qemu_bh_new(virtio_gpu_cursor_bh, g);
    QTAILQ_INIT(&g->reslist);
    QTAILQ_INIT(&g->cmdq);
    QTAILQ_INIT(&g->fenceq);
}

static void virtio_gpu_reset(VirtIODevice *vdev)
{
    VirtIOGPU *g = VIRTIO_GPU(vdev);
    struct virtio_gpu_simple_resource *res, *tmp;
    struct virtio_gpu_ctrl_command *cmd;

#ifdef CONFIG_VIRGL
    if (g->parent_obj.use_virgl_renderer) {
        virtio_gpu_virgl_reset(g);
    }
#endif

    QTAILQ_FOREACH_SAFE(res, &g->reslist, next, tmp) {
        virtio_gpu_resource_destroy(g, res);
    }

    while (!QTAILQ_EMPTY(&g->cmdq)) {
        cmd = QTAILQ_FIRST(&g->cmdq);
        QTAILQ_REMOVE(&g->cmdq, cmd, next);
        g_free(cmd);
    }

    while (!QTAILQ_EMPTY(&g->fenceq)) {
        cmd = QTAILQ_FIRST(&g->fenceq);
        QTAILQ_REMOVE(&g->fenceq, cmd, next);
        g->inflight--;
        g_free(cmd);
    }

#ifdef CONFIG_VIRGL
    if (g->parent_obj.use_virgl_renderer) {
        if (g->parent_obj.renderer_blocked) {
            g->renderer_reset = true;
        } else {
            virtio_gpu_virgl_reset(g);
        }
        g->parent_obj.use_virgl_renderer = false;
    }
#endif

    virtio_gpu_base_reset(VIRTIO_GPU_BASE(vdev));
}

static void
virtio_gpu_get_config(VirtIODevice *vdev, uint8_t *config)
{
    VirtIOGPUBase *g = VIRTIO_GPU_BASE(vdev);

    memcpy(config, &g->virtio_config, sizeof(g->virtio_config));
}

static void
virtio_gpu_set_config(VirtIODevice *vdev, const uint8_t *config)
{
    VirtIOGPUBase *g = VIRTIO_GPU_BASE(vdev);
    const struct virtio_gpu_config *vgconfig =
        (const struct virtio_gpu_config *)config;

    if (vgconfig->events_clear) {
        g->virtio_config.events_read &= ~vgconfig->events_clear;
    }
}

/*
 * For historical reasons virtio_gpu does not adhere to virtio migration
 * scheme as described in doc/virtio-migration.txt, in a sense that no
 * save/load callback are provided to the core. Instead the device data
 * is saved/loaded after the core data.
 *
 * Because of this we need a special vmsd.
 */
static const VMStateDescription vmstate_virtio_gpu = {
    .name = "virtio-gpu",
    .minimum_version_id = VIRTIO_GPU_VM_VERSION,
    .version_id = VIRTIO_GPU_VM_VERSION,
    .fields = (VMStateField[]) {
        VMSTATE_VIRTIO_DEVICE /* core */,
        {
            .name = "virtio-gpu",
            .info = &(const VMStateInfo) {
                        .name = "virtio-gpu",
                        .get = virtio_gpu_load,
                        .put = virtio_gpu_save,
            },
            .flags = VMS_SINGLE,
        } /* device */,
        VMSTATE_END_OF_LIST()
    },
};

static Property virtio_gpu_properties[] = {
    VIRTIO_GPU_BASE_PROPERTIES(VirtIOGPU, parent_obj.conf),
    DEFINE_PROP_SIZE("max_hostmem", VirtIOGPU, conf_max_hostmem,
                     256 * MiB),
#ifdef CONFIG_VIRGL
    DEFINE_PROP_BIT("virgl", VirtIOGPU, parent_obj.conf.flags,
                    VIRTIO_GPU_FLAG_VIRGL_ENABLED, true),
    DEFINE_PROP_BIT("stats", VirtIOGPU, parent_obj.conf.flags,
                    VIRTIO_GPU_FLAG_STATS_ENABLED, false),
#endif
    DEFINE_PROP_END_OF_LIST(),
};

static void virtio_gpu_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioDeviceClass *vdc = VIRTIO_DEVICE_CLASS(klass);
    VirtIOGPUBaseClass *vgc = VIRTIO_GPU_BASE_CLASS(klass);

    vgc->gl_flushed = virtio_gpu_gl_flushed;
    vdc->realize = virtio_gpu_device_realize;
    vdc->reset = virtio_gpu_reset;
    vdc->get_config = virtio_gpu_get_config;
    vdc->set_config = virtio_gpu_set_config;

    dc->vmsd = &vmstate_virtio_gpu;
    device_class_set_props(dc, virtio_gpu_properties);
}

static const TypeInfo virtio_gpu_info = {
    .name = TYPE_VIRTIO_GPU,
    .parent = TYPE_VIRTIO_GPU_BASE,
    .instance_size = sizeof(VirtIOGPU),
    .class_init = virtio_gpu_class_init,
};

static void virtio_register_types(void)
{
    type_register_static(&virtio_gpu_info);
}

type_init(virtio_register_types)
