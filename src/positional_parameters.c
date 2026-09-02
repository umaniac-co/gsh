#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif
#if _POSIX_C_SOURCE < 202405L
#error "gsh requires the POSIX.1-2024 feature-test baseline"
#endif

#include "positional_parameters.h"

#include <errno.h>
#include <string.h>

void gsh_positionals_initialize(gsh_positional_store *store)
{
    if (store == NULL) {
        return;
    }
    store->version = GSH_POSITIONAL_VERSION;
    store->count = 0;
    store->start = 0;
    store->text_used = 0;
}

int gsh_positionals_assign(gsh_positional_store *store, size_t count,
                           char *const values[])
{
    if (store == NULL || (count != 0U && values == NULL)) {
        return -1;
    }
    size_t lengths[GSH_POSITIONAL_CAP];
    size_t used = 0;
    size_t index;

    if (count > GSH_POSITIONAL_CAP) {
        errno = E2BIG;
        return -1;
    }
    for (index = 0; index < count; index++) {
        size_t remaining = GSH_POSITIONAL_TEXT_CAP - used;
        size_t length = strnlen(values[index], remaining);

        if (length == remaining) {
            errno = E2BIG;
            return -1;
        }
        lengths[index] = length;
        used += length + 1U;
    }
    gsh_positionals_initialize(store);
    for (index = 0; index < count; index++) {
        store->offsets[index] = (uint16_t)store->text_used;
        (void)memcpy(store->text + store->text_used, values[index],
               lengths[index] + 1U);
        store->text_used += (uint32_t)lengths[index] + 1U;
    }
    store->count = (uint32_t)count;
    return 0;
}

int gsh_positionals_shift(gsh_positional_store *store, size_t amount)
{
    if (store == NULL) return -1;
    if (amount > store->count) {
        errno = EINVAL;
        return -1;
    }
    if (amount == store->count) {
        gsh_positionals_initialize(store);
    } else {
        store->start += (uint32_t)amount;
        store->count -= (uint32_t)amount;
    }
    return 0;
}

size_t gsh_positionals_count(const gsh_positional_store *store)
{
    if (store == NULL) {
        return 0U;
    }
    return store == NULL ? 0 : store->count;
}

void gsh_positionals_view(const gsh_positional_store *store,
                          char *values[GSH_POSITIONAL_CAP])
{
    if (store == NULL || values == NULL) {
        return;
    }
    size_t index;
    size_t count = gsh_positionals_count(store);

    for (index = 0; index < count; index++) {
        values[index] = (char *)store->text +
                        store->offsets[store->start + index];
    }
}

bool gsh_positionals_validate(const gsh_positional_store *store)
{
    size_t index;

    if (store == NULL || store->version != GSH_POSITIONAL_VERSION ||
        store->count > GSH_POSITIONAL_CAP ||
        store->start > GSH_POSITIONAL_CAP - store->count ||
        store->text_used > GSH_POSITIONAL_TEXT_CAP) {
        return false;
    }
    for (index = 0; index < store->count; index++) {
        size_t offset = store->offsets[store->start + index];

        if (offset >= store->text_used ||
            memchr(store->text + offset, '\0',
                   store->text_used - offset) == NULL) {
            return false;
        }
    }
    return true;
}
