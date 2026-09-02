#ifndef GSH_RESOURCE_PROTOCOL_H
#define GSH_RESOURCE_PROTOCOL_H

#include <stdint.h>

enum {
    GSH_RESOURCE_PROTOCOL_VERSION = 2,
    GSH_RESOURCE_PROTOCOL_PATH_CAP = 4096,
    GSH_RESOURCE_PROTOCOL_LABEL_CAP = 512,
    GSH_RESOURCE_PROTOCOL_NAVIGABLE = 1,
    GSH_RESOURCE_PROTOCOL_VIEW_FULL = 1,
    GSH_RESOURCE_PROTOCOL_VIEW_SPLIT = 2,
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

#endif
