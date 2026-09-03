#ifndef GSH_RESOURCE_PROTOCOL_H
#define GSH_RESOURCE_PROTOCOL_H

#include <stdint.h>

enum {
    GSH_RESOURCE_PROTOCOL_VERSION = 3,
    GSH_RESOURCE_PROTOCOL_PATH_CAP = 4096,
    GSH_RESOURCE_PROTOCOL_LABEL_CAP = 512,
    GSH_PREVIEW_FRAME_CHUNK_CAP = 2048,
    GSH_PREVIEW_FRAME_TOTAL_CAP = 4 * 1024 * 1024,
    GSH_RESOURCE_PROTOCOL_NAVIGABLE = 1,
    GSH_RESOURCE_PROTOCOL_VIEW_FULL = 1,
    GSH_RESOURCE_PROTOCOL_VIEW_SPLIT = 2,
    GSH_RESOURCE_PROTOCOL_FRAME_BEGIN = 3,
    GSH_RESOURCE_PROTOCOL_FRAME_CHUNK = 4,
    GSH_RESOURCE_PROTOCOL_FRAME_END = 5,
    GSH_RESOURCE_PROTOCOL_FRAME_DELETE = 6,
    GSH_RESOURCE_PROTOCOL_FRAME_PNG = 1,
    GSH_RESOURCE_PROTOCOL_FRAME_JPEG = 2,
    GSH_RESOURCE_PROTOCOL_FRAME_GIF = 3,
    GSH_RESOURCE_PROTOCOL_FRAME_SIXEL = 4,
    GSH_RESOURCE_IMAGE_NONE = 0,
    GSH_RESOURCE_IMAGE_KITTY = 1,
    GSH_RESOURCE_IMAGE_ITERM = 2,
    GSH_RESOURCE_IMAGE_SIXEL = 3,
};

typedef struct {
    uint32_t version;
    uint32_t size;
    uint32_t event;
    uint32_t separator_column;
} gsh_resource_view_record;

typedef struct {
    uint32_t version;
    uint32_t size;
    uint32_t row;
    uint32_t byte_begin;
    uint32_t byte_end;
    uint32_t column_begin;
    uint32_t column_end;
    uint32_t type;
    uint32_t flags;
    uint32_t path_length;
    uint32_t label_length;
} gsh_resource_record_header;

typedef struct {
    uint32_t version;
    uint32_t size;
    uint32_t event;
    uint32_t generation;
    uint32_t frame_id;
    uint32_t format;
    uint32_t pixel_width;
    uint32_t pixel_height;
    uint32_t cell_row;
    uint32_t cell_column;
    uint32_t cell_rows;
    uint32_t cell_columns;
    uint32_t total_length;
    uint32_t offset;
    uint32_t chunk_length;
    uint32_t reserved;
} gsh_preview_frame_record;

#endif
