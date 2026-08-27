#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif
#if _POSIX_C_SOURCE < 202405L
#error "gsh requires the POSIX.1-2024 feature-test baseline"
#endif

#include "shell_functions.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    size_t nodes;
    size_t words;
    size_t redirects;
    size_t source_begin;
    size_t source_end;
    size_t visited;
} function_measure;

typedef struct {
    const gsh_parse_storage *source;
    size_t source_begin;
    size_t source_end;
    gsh_parse_storage *destination;
    size_t destination_source;
    size_t next_node;
    size_t next_word;
    size_t next_redirect;
    size_t node_end;
    size_t word_end;
    size_t redirect_end;
} function_copy;

static uint32_t hash_name(const char *name, size_t length)
{
    uint32_t hash = UINT32_C(2166136261);
    size_t index;

    for (index = 0; index < length; index++) {
        hash ^= (unsigned char)name[index];
        hash *= UINT32_C(16777619);
    }
    return hash;
}

static size_t find_index(const gsh_function_store *store,
                         const char *name, size_t name_length,
                         uint32_t hash)
{
    size_t slot = hash & (GSH_FUNCTION_HASH_CAP - 1U);
    size_t probes;
    const char *text = gsh_functions_text(store);

    for (probes = 0; probes < GSH_FUNCTION_HASH_CAP; probes++) {
        uint16_t stored = store->hash_slots[slot];

        if (stored == 0) {
            return GSH_FUNCTION_CAP;
        }
        if (store->entries[stored - 1U].hash == hash &&
            store->entries[stored - 1U].name_length == name_length &&
            memcmp(text + store->entries[stored - 1U].source_offset,
                   name, name_length) == 0) {
            return stored - 1U;
        }
        slot = (slot + 1U) & (GSH_FUNCTION_HASH_CAP - 1U);
    }
    return GSH_FUNCTION_CAP;
}

static void rebuild_hash(gsh_function_store *store)
{
    size_t index;

    memset(store->hash_slots, 0, sizeof(store->hash_slots));
    for (index = 0; index < store->count; index++) {
        size_t slot = store->entries[index].hash &
                      (GSH_FUNCTION_HASH_CAP - 1U);

        while (store->hash_slots[slot] != 0) {
            slot = (slot + 1U) & (GSH_FUNCTION_HASH_CAP - 1U);
        }
        store->hash_slots[slot] = (uint16_t)(index + 1U);
    }
}

static bool include_reference(function_measure *measure,
                              gsh_word_ref reference,
                              size_t input_length)
{
    if (reference.begin > reference.end ||
        reference.end > input_length ||
        reference.begin < measure->source_begin) {
        return false;
    }
    if (reference.end > measure->source_end) {
        measure->source_end = reference.end;
    }
    return true;
}

static bool measure_node(const gsh_parse_storage *storage,
                         size_t node_index, size_t input_length,
                         size_t depth, function_measure *measure)
{
    const gsh_ast_node *node;
    size_t index;
    size_t child;

    if (depth > GSH_PARSE_NODE_CAP ||
        node_index >= storage->node_count ||
        ++measure->visited > storage->node_count) {
        return false;
    }
    node = &storage->nodes[node_index];
    if (node->begin < measure->source_begin || node->end > input_length ||
        node->begin > node->end ||
        node->first_word > storage->word_count ||
        node->word_count > storage->word_count - node->first_word ||
        node->first_redirect > storage->redirect_count ||
        node->redirect_count >
            storage->redirect_count - node->first_redirect) {
        return false;
    }
    measure->nodes++;
    measure->words += node->word_count;
    measure->redirects += node->redirect_count;
    if (node->end > measure->source_end) {
        measure->source_end = node->end;
    }
    for (index = 0; index < node->word_count; index++) {
        if (!include_reference(
                measure, storage->words[node->first_word + index],
                input_length)) {
            return false;
        }
    }
    for (index = 0; index < node->redirect_count; index++) {
        const gsh_redirect *redirect =
            &storage->redirects[node->first_redirect + index];

        if (!include_reference(measure, redirect->target, input_length) ||
            !include_reference(measure, redirect->body, input_length)) {
            return false;
        }
    }
    child = node->first_child;
    while (child != GSH_AST_NONE) {
        size_t next;

        if (child >= storage->node_count) {
            return false;
        }
        next = storage->nodes[child].next_sibling;
        if (!measure_node(storage, child, input_length, depth + 1U,
                          measure)) {
            return false;
        }
        child = next;
    }
    return true;
}

static bool adjust_reference(const function_copy *copy,
                             gsh_word_ref source,
                             gsh_word_ref *destination)
{
    if (source.begin < copy->source_begin ||
        source.end > copy->source_end || source.begin > source.end) {
        return false;
    }
    destination->begin = copy->destination_source +
                         source.begin - copy->source_begin;
    destination->end = copy->destination_source +
                       source.end - copy->source_begin;
    return true;
}

static size_t copy_node(function_copy *copy, size_t source_index,
                        size_t depth)
{
    const gsh_ast_node *source;
    gsh_ast_node *destination;
    size_t destination_index;
    size_t index;
    size_t child;

    if (depth > GSH_PARSE_NODE_CAP ||
        source_index >= copy->source->node_count ||
        copy->next_node == copy->node_end) {
        return GSH_AST_NONE;
    }
    source = &copy->source->nodes[source_index];
    destination_index = copy->next_node++;
    destination = &copy->destination->nodes[destination_index];
    *destination = *source;
    destination->first_child = GSH_AST_NONE;
    destination->last_child = GSH_AST_NONE;
    destination->next_sibling = GSH_AST_NONE;
    destination->first_word = copy->next_word;
    destination->first_redirect = copy->next_redirect;
    destination->begin = copy->destination_source +
                         source->begin - copy->source_begin;
    destination->end = copy->destination_source +
                       source->end - copy->source_begin;
    if (source->word_count > copy->word_end - copy->next_word ||
        source->redirect_count >
            copy->redirect_end - copy->next_redirect) {
        return GSH_AST_NONE;
    }
    for (index = 0; index < source->word_count; index++) {
        if (!adjust_reference(
                copy, copy->source->words[source->first_word + index],
                &copy->destination->words[copy->next_word++])) {
            return GSH_AST_NONE;
        }
    }
    for (index = 0; index < source->redirect_count; index++) {
        const gsh_redirect *source_redirect =
            &copy->source->redirects[source->first_redirect + index];
        gsh_redirect *destination_redirect =
            &copy->destination->redirects[copy->next_redirect++];

        *destination_redirect = *source_redirect;
        if (!adjust_reference(copy, source_redirect->target,
                              &destination_redirect->target) ||
            !adjust_reference(copy, source_redirect->body,
                              &destination_redirect->body)) {
            return GSH_AST_NONE;
        }
    }
    child = source->first_child;
    while (child != GSH_AST_NONE) {
        size_t next = copy->source->nodes[child].next_sibling;
        size_t copied = copy_node(copy, child, depth + 1U);

        if (copied == GSH_AST_NONE) {
            return GSH_AST_NONE;
        }
        if (destination->first_child == GSH_AST_NONE) {
            destination->first_child = copied;
        } else {
            copy->destination->nodes[destination->last_child].next_sibling =
                copied;
        }
        destination->last_child = copied;
        child = next;
    }
    return destination_index;
}

static int set_once(gsh_function_store *store, const char *input,
                    size_t input_length,
                    const gsh_parse_storage *storage,
                    size_t function_node, bool preserve_active_programs)
{
    const gsh_ast_node *node;
    gsh_word_ref name;
    function_measure measure = {0};
    function_copy copy;
    gsh_function_entry allocation;
    gsh_function_entry *entry;
    size_t source_length;
    size_t existing;
    size_t copied;
    uint32_t hash;

    if (function_node >= storage->node_count) {
        errno = EINVAL;
        return -1;
    }
    node = &storage->nodes[function_node];
    if (node->kind != GSH_AST_FUNCTION || node->word_count != 1U ||
        node->first_child == GSH_AST_NONE ||
        node->first_word >= storage->word_count) {
        errno = EINVAL;
        return -1;
    }
    name = storage->words[node->first_word];
    if (name.begin != node->begin || name.begin == name.end ||
        name.end > input_length || name.end - name.begin > UINT16_MAX) {
        errno = EINVAL;
        return -1;
    }
    measure.source_begin = node->begin;
    measure.source_end = node->end;
    if (!measure_node(storage, function_node, input_length, 0, &measure)) {
        errno = EINVAL;
        return -1;
    }
    source_length = measure.source_end - measure.source_begin;
    hash = hash_name(input + name.begin, name.end - name.begin);
    existing = find_index(store, input + name.begin,
                          name.end - name.begin, hash);
    if (existing != GSH_FUNCTION_CAP && !preserve_active_programs &&
        source_length <= store->entries[existing].source_capacity &&
        measure.nodes <= store->entries[existing].node_capacity &&
        measure.words <= store->entries[existing].word_capacity &&
        measure.redirects <=
            store->entries[existing].redirect_capacity) {
        allocation = store->entries[existing];
    } else {
        if (source_length > GSH_FUNCTION_TEXT_CAP - store->text_used ||
            measure.nodes > GSH_PARSE_NODE_CAP -
                                store->programs.node_count ||
            measure.words > GSH_PARSE_WORD_CAP -
                                store->programs.word_count ||
            measure.redirects > GSH_PARSE_REDIRECT_CAP -
                                    store->programs.redirect_count ||
            (existing == GSH_FUNCTION_CAP &&
             store->count == GSH_FUNCTION_CAP)) {
            errno = ENOSPC;
            return -1;
        }
        memset(&allocation, 0, sizeof(allocation));
        allocation.source_offset = store->text_used;
        allocation.source_capacity = (uint32_t)source_length;
        allocation.node_offset = (uint32_t)store->programs.node_count;
        allocation.node_capacity = (uint32_t)measure.nodes;
        allocation.word_offset = (uint32_t)store->programs.word_count;
        allocation.word_capacity = (uint32_t)measure.words;
        allocation.redirect_offset =
            (uint32_t)store->programs.redirect_count;
        allocation.redirect_capacity = (uint32_t)measure.redirects;
    }
    allocation.source_length = (uint32_t)source_length;
    memmove((char *)store->programs.tokens + allocation.source_offset,
            input + measure.source_begin, source_length);
    copy.source = storage;
    copy.source_begin = measure.source_begin;
    copy.source_end = measure.source_end;
    copy.destination = &store->programs;
    copy.destination_source = allocation.source_offset;
    copy.next_node = allocation.node_offset;
    copy.next_word = allocation.word_offset;
    copy.next_redirect = allocation.redirect_offset;
    copy.node_end = allocation.node_offset + allocation.node_capacity;
    copy.word_end = allocation.word_offset + allocation.word_capacity;
    copy.redirect_end = allocation.redirect_offset +
                        allocation.redirect_capacity;
    copied = copy_node(&copy, function_node, 0);
    if (copied == GSH_AST_NONE || copied != allocation.node_offset) {
        errno = EINVAL;
        return -1;
    }
    allocation.hash = hash;
    allocation.name_length = (uint16_t)(name.end - name.begin);
    if (existing == GSH_FUNCTION_CAP) {
        entry = &store->entries[store->count++];
    } else {
        entry = &store->entries[existing];
    }
    *entry = allocation;
    if (allocation.source_offset + allocation.source_capacity >
        store->text_used) {
        store->text_used = allocation.source_offset +
                           allocation.source_capacity;
    }
    if (copy.next_node > store->programs.node_count) {
        store->programs.node_count = copy.next_node;
    }
    if (copy.next_word > store->programs.word_count) {
        store->programs.word_count = copy.next_word;
    }
    if (copy.next_redirect > store->programs.redirect_count) {
        store->programs.redirect_count = copy.next_redirect;
    }
    rebuild_hash(store);
    return 0;
}

static int compact(gsh_function_store *store,
                   gsh_function_store *scratch)
{
    size_t index;
    size_t old_count = store->count;
    const char *text = gsh_functions_text(store);

    gsh_functions_initialize(scratch);
    for (index = 0; index < store->count; index++) {
        if (set_once(scratch, text, store->text_used, &store->programs,
                     store->entries[index].node_offset, false) == -1) {
            return -1;
        }
    }
    memcpy(store->entries, scratch->entries,
           scratch->count * sizeof(store->entries[0]));
    if (old_count > scratch->count) {
        memset(store->entries + scratch->count, 0,
               (old_count - scratch->count) * sizeof(store->entries[0]));
    }
    memcpy(store->hash_slots, scratch->hash_slots,
           sizeof(store->hash_slots));
    store->count = scratch->count;
    store->text_used = scratch->text_used;
    memcpy(store->programs.tokens, scratch->programs.tokens,
           scratch->text_used);
    memcpy(store->programs.nodes, scratch->programs.nodes,
           scratch->programs.node_count *
               sizeof(store->programs.nodes[0]));
    memcpy(store->programs.words, scratch->programs.words,
           scratch->programs.word_count *
               sizeof(store->programs.words[0]));
    memcpy(store->programs.redirects, scratch->programs.redirects,
           scratch->programs.redirect_count *
               sizeof(store->programs.redirects[0]));
    store->programs.token_count = 0;
    store->programs.node_count = scratch->programs.node_count;
    store->programs.word_count = scratch->programs.word_count;
    store->programs.redirect_count = scratch->programs.redirect_count;
    store->programs.heredoc_count = 0;
    return 0;
}

void gsh_functions_initialize(gsh_function_store *store)
{
    memset(store->entries, 0, sizeof(store->entries));
    memset(store->hash_slots, 0, sizeof(store->hash_slots));
    store->count = 0;
    store->text_used = 0;
    store->programs.token_count = 0;
    store->programs.node_count = 0;
    store->programs.word_count = 0;
    store->programs.redirect_count = 0;
    store->programs.heredoc_count = 0;
}

const gsh_function_entry *gsh_functions_lookup(
    const gsh_function_store *store, const char *name,
    size_t name_length)
{
    uint32_t hash;
    size_t index;

    if (store == NULL || name == NULL || name_length == 0) {
        return NULL;
    }
    hash = hash_name(name, name_length);
    index = find_index(store, name, name_length, hash);
    return index == GSH_FUNCTION_CAP ? NULL : &store->entries[index];
}

int gsh_functions_set(gsh_function_store *store,
                      gsh_function_store *scratch,
                      const char *input, size_t input_length,
                      const gsh_parse_storage *storage,
                      size_t function_node,
                      int preserve_active_programs)
{
    if (store == NULL || input == NULL || storage == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (set_once(store, input, input_length, storage, function_node,
                 preserve_active_programs != 0) == 0) {
        return 0;
    }
    if (errno != ENOSPC || scratch == NULL || preserve_active_programs ||
        compact(store, scratch) == -1) {
        return -1;
    }
    return set_once(store, input, input_length, storage, function_node,
                    false);
}

int gsh_functions_unset(gsh_function_store *store,
                        const char *name, size_t name_length)
{
    size_t index;

    if (store == NULL || name == NULL || name_length == 0) {
        errno = EINVAL;
        return -1;
    }
    index = find_index(store, name, name_length,
                       hash_name(name, name_length));
    if (index == GSH_FUNCTION_CAP) {
        return 0;
    }
    store->count--;
    if (index != store->count) {
        store->entries[index] = store->entries[store->count];
    }
    memset(&store->entries[store->count], 0,
           sizeof(store->entries[store->count]));
    rebuild_hash(store);
    return 0;
}

size_t gsh_functions_count(const gsh_function_store *store)
{
    return store == NULL ? 0 : store->count;
}

const char *gsh_functions_text(const gsh_function_store *store)
{
    return (const char *)store->programs.tokens;
}

void gsh_functions_snapshot_header(
    const gsh_function_store *store, uint64_t base_generation,
    gsh_function_snapshot_header *header)
{
    header->version = GSH_FUNCTION_SNAPSHOT_VERSION;
    header->count = store->count;
    header->text_used = store->text_used;
    header->node_count = (uint32_t)store->programs.node_count;
    header->word_count = (uint32_t)store->programs.word_count;
    header->redirect_count = (uint32_t)store->programs.redirect_count;
    header->reserved = 0;
    header->base_generation = base_generation;
}

bool gsh_functions_snapshot_header_valid(
    const gsh_function_snapshot_header *header)
{
    return header != NULL &&
           header->version == GSH_FUNCTION_SNAPSHOT_VERSION &&
           header->reserved == 0 && header->count <= GSH_FUNCTION_CAP &&
           header->text_used <= GSH_FUNCTION_TEXT_CAP &&
           header->node_count <= GSH_PARSE_NODE_CAP &&
           header->word_count <= GSH_PARSE_WORD_CAP &&
           header->redirect_count <= GSH_PARSE_REDIRECT_CAP;
}

size_t gsh_functions_snapshot_payload_size(
    const gsh_function_snapshot_header *header)
{
    if (!gsh_functions_snapshot_header_valid(header)) {
        return 0;
    }
    return (size_t)header->count * sizeof(gsh_function_entry) +
           (size_t)header->text_used +
           (size_t)header->node_count * sizeof(gsh_ast_node) +
           (size_t)header->word_count * sizeof(gsh_word_ref) +
           (size_t)header->redirect_count * sizeof(gsh_redirect);
}

static const void *snapshot_section_source(
    const gsh_function_store *store,
    const gsh_function_snapshot_header *header, size_t offset,
    size_t *available)
{
    const void *sections[] = {
        store->entries, store->programs.tokens, store->programs.nodes,
        store->programs.words, store->programs.redirects};
    const size_t lengths[] = {
        (size_t)header->count * sizeof(gsh_function_entry),
        (size_t)header->text_used,
        (size_t)header->node_count * sizeof(gsh_ast_node),
        (size_t)header->word_count * sizeof(gsh_word_ref),
        (size_t)header->redirect_count * sizeof(gsh_redirect)};
    size_t index;

    for (index = 0; index < sizeof(lengths) / sizeof(lengths[0]); index++) {
        if (offset < lengths[index]) {
            *available = lengths[index] - offset;
            return (const unsigned char *)sections[index] + offset;
        }
        offset -= lengths[index];
    }
    *available = 0;
    return NULL;
}

const void *gsh_functions_snapshot_source(
    const gsh_function_store *store,
    const gsh_function_snapshot_header *header, size_t offset,
    size_t *available)
{
    if (store == NULL || available == NULL ||
        !gsh_functions_snapshot_header_valid(header) ||
        offset >= gsh_functions_snapshot_payload_size(header)) {
        if (available != NULL) {
            *available = 0;
        }
        return NULL;
    }
    return snapshot_section_source(store, header, offset, available);
}

void *gsh_functions_snapshot_destination(
    gsh_function_store *store,
    const gsh_function_snapshot_header *header, size_t offset,
    size_t *available)
{
    return (void *)gsh_functions_snapshot_source(store, header, offset,
                                                  available);
}

static bool function_name_is_valid(const char *name, size_t length)
{
    size_t index;

    if (length == 0 || !((name[0] >= 'A' && name[0] <= 'Z') ||
                         (name[0] >= 'a' && name[0] <= 'z') ||
                         name[0] == '_')) {
        return false;
    }
    for (index = 1; index < length; index++) {
        if (!((name[index] >= 'A' && name[index] <= 'Z') ||
              (name[index] >= 'a' && name[index] <= 'z') ||
              (name[index] >= '0' && name[index] <= '9') ||
              name[index] == '_')) {
            return false;
        }
    }
    return true;
}

static bool reference_in_function(const gsh_function_entry *entry,
                                  gsh_word_ref reference)
{
    size_t end = (size_t)entry->source_offset + entry->source_length;

    return reference.begin <= reference.end &&
           reference.begin >= entry->source_offset && reference.end <= end;
}

static bool validate_snapshot_node(const gsh_function_store *store,
                                   const gsh_function_entry *entry,
                                   size_t node_index, size_t depth,
                                   size_t *visited)
{
    const gsh_ast_node *node;
    size_t node_end = (size_t)entry->node_offset + entry->node_capacity;
    size_t word_end = (size_t)entry->word_offset + entry->word_capacity;
    size_t redirect_end = (size_t)entry->redirect_offset +
                          entry->redirect_capacity;
    size_t child;
    size_t index;

    if (depth > entry->node_capacity || node_index < entry->node_offset ||
        node_index >= node_end || ++*visited > entry->node_capacity) {
        return false;
    }
    node = &store->programs.nodes[node_index];
    if (node->begin > node->end || node->begin < entry->source_offset ||
        node->end > (size_t)entry->source_offset + entry->source_length ||
        node->first_word < entry->word_offset ||
        node->first_word > word_end || node->word_count >
                                               word_end - node->first_word ||
        node->first_redirect < entry->redirect_offset ||
        node->first_redirect > redirect_end || node->redirect_count >
                                                   redirect_end -
                                                       node->first_redirect) {
        return false;
    }
    for (index = 0; index < node->word_count; index++) {
        if (!reference_in_function(
                entry, store->programs.words[node->first_word + index])) {
            return false;
        }
    }
    for (index = 0; index < node->redirect_count; index++) {
        const gsh_redirect *redirect =
            &store->programs.redirects[node->first_redirect + index];

        if (!reference_in_function(entry, redirect->target) ||
            !reference_in_function(entry, redirect->body)) {
            return false;
        }
    }
    child = node->first_child;
    while (child != GSH_AST_NONE) {
        size_t next;

        if (child < entry->node_offset || child >= node_end) {
            return false;
        }
        next = store->programs.nodes[child].next_sibling;
        if (!validate_snapshot_node(store, entry, child, depth + 1U,
                                    visited) ||
            (next != GSH_AST_NONE &&
             (next < entry->node_offset || next >= node_end))) {
            return false;
        }
        child = next;
    }
    return true;
}

static bool validate_snapshot_store(const gsh_function_store *store)
{
    const char *text = gsh_functions_text(store);
    size_t index;

    for (index = 0; index < store->count; index++) {
        const gsh_function_entry *entry = &store->entries[index];
        const gsh_ast_node *root;
        gsh_word_ref name;
        size_t visited = 0;
        size_t other;

        if (entry->reserved != 0 || entry->name_length == 0 ||
            entry->source_length < entry->name_length ||
            entry->source_length > entry->source_capacity ||
            entry->source_offset > store->text_used ||
            entry->source_capacity >
                store->text_used - entry->source_offset ||
            entry->node_offset > store->programs.node_count ||
            entry->node_capacity >
                store->programs.node_count - entry->node_offset ||
            entry->word_offset > store->programs.word_count ||
            entry->word_capacity >
                store->programs.word_count - entry->word_offset ||
            entry->redirect_offset > store->programs.redirect_count ||
            entry->redirect_capacity >
                store->programs.redirect_count - entry->redirect_offset ||
            entry->node_capacity == 0 || entry->word_capacity == 0 ||
            !function_name_is_valid(text + entry->source_offset,
                                    entry->name_length) ||
            entry->hash != hash_name(text + entry->source_offset,
                                     entry->name_length) ||
            !validate_snapshot_node(store, entry, entry->node_offset, 0,
                                    &visited)) {
            return false;
        }
        root = &store->programs.nodes[entry->node_offset];
        if (root->kind != GSH_AST_FUNCTION || root->word_count != 1U ||
            root->first_child == GSH_AST_NONE) {
            return false;
        }
        name = store->programs.words[root->first_word];
        if (name.begin != entry->source_offset ||
            name.end - name.begin != entry->name_length) {
            return false;
        }
        for (other = 0; other < index; other++) {
            const gsh_function_entry *prior = &store->entries[other];

            if (prior->name_length == entry->name_length &&
                memcmp(text + prior->source_offset,
                       text + entry->source_offset,
                       entry->name_length) == 0) {
                return false;
            }
        }
    }
    return true;
}

bool gsh_functions_snapshot_finalize(
    gsh_function_store *store,
    const gsh_function_snapshot_header *header)
{
    if (store == NULL || !gsh_functions_snapshot_header_valid(header)) {
        return false;
    }
    store->count = header->count;
    store->text_used = header->text_used;
    store->programs.token_count = 0;
    store->programs.node_count = header->node_count;
    store->programs.word_count = header->word_count;
    store->programs.redirect_count = header->redirect_count;
    store->programs.heredoc_count = 0;
    if (!validate_snapshot_store(store)) {
        return false;
    }
    rebuild_hash(store);
    return true;
}

bool gsh_functions_clone(gsh_function_store *destination,
                         const gsh_function_store *source)
{
    gsh_function_snapshot_header header;
    size_t offset = 0;
    size_t total;

    if (destination == NULL || source == NULL || destination == source) {
        return false;
    }
    gsh_functions_snapshot_header(source, 0, &header);
    total = gsh_functions_snapshot_payload_size(&header);
    while (offset < total) {
        size_t source_available;
        size_t destination_available;
        const void *from = gsh_functions_snapshot_source(
            source, &header, offset, &source_available);
        void *to = gsh_functions_snapshot_destination(
            destination, &header, offset, &destination_available);
        size_t amount = source_available < destination_available
                            ? source_available
                            : destination_available;

        if (from == NULL || to == NULL || amount == 0) {
            return false;
        }
        memcpy(to, from, amount);
        offset += amount;
    }
    return gsh_functions_snapshot_finalize(destination, &header);
}
