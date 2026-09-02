#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif
#if _POSIX_C_SOURCE < 202405L
#error "gsh fuzzers require the POSIX.1-2024 feature-test baseline"
#endif

#include "../src/native_plan.h"
#include "../src/alias_expansion.h"
#include "../src/shell_functions.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool require_at(int condition, int line)
{
    if (!condition) {
        (void)fprintf(stderr, "fuzz invariant failed at line %d\n", line);
        return false;
    }
    return true;
}

/* ── Macro Assertion Adapter Is a Declared Test Root ───────────
 * Fuzz invariants need the source line while callers need expression syntax.
 * The macro therefore provides only location data and delegates all behavior.
 * Both standalone and libFuzzer targets reach this same concrete adapter.
 * The bounded call graph names the adapter because it does not preprocess C.
 * ──────────────────────────────────────────────────────── */
#define require(condition)                                                   \
    do {                                                                     \
        if (!require_at((condition), __LINE__)) return false;                \
    } while (false)

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static bool pointer_offset(const void *pointer, const void *base,
                           size_t length, bool allow_end, size_t *offset)
{
    if (base == NULL || offset == NULL || pointer == NULL) {
        return false;
    }
    uintptr_t address = (uintptr_t)pointer;
    uintptr_t begin = (uintptr_t)base;

    require(base != NULL);
    require(offset != NULL);
    if (address < begin || address - begin > length ||
        (!allow_end && address - begin == length)) {
        return false;
    }
    *offset = (size_t)(address - begin);
    return true;
}

typedef struct {
    gsh_parse_storage parse_storage;
    gsh_native_pipeline native_pipeline;
    gsh_alias_store aliases;
    char alias_expansion[GSH_ALIAS_EXPANSION_CAP];
    gsh_function_store functions;
    gsh_function_store function_scratch;
    gsh_function_store function_snapshot;
} fuzz_workspace;

_Static_assert(sizeof(((fuzz_workspace *)0)->alias_expansion) ==
                   GSH_ALIAS_EXPANSION_CAP,
               "fuzz alias expansion retains the production capacity");
_Static_assert(sizeof(((fuzz_workspace *)0)->parse_storage.tokens) ==
                   sizeof(((gsh_parse_storage *)0)->tokens),
               "fuzz parsing uses the complete production token arena");

static fuzz_workspace *fuzz_storage(void)
{
    static fuzz_workspace storage;

    return &storage;
}

static bool fuzz_hostile_function_header(
    const uint8_t *data, size_t size,
    const gsh_function_snapshot_header *valid_header)
{
    if (data == NULL || valid_header == NULL) {
        return false;
    }
    fuzz_workspace *workspace = fuzz_storage();
    gsh_function_snapshot_header hostile;

    require(workspace != NULL);
    require(data != NULL && valid_header != NULL);
    require(size > 0);
    hostile = *valid_header;
    switch (data[size - 1U] % 7U) {
    case 0: hostile.version++; break;
    case 1: hostile.count = UINT32_MAX; break;
    case 2: hostile.text_used = UINT32_MAX; break;
    case 3: hostile.node_count = UINT32_MAX; break;
    case 4: hostile.word_count = UINT32_MAX; break;
    case 5: hostile.redirect_count = UINT32_MAX; break;
    default: hostile.reserved = 1; break;
    }
    require(!gsh_functions_snapshot_header_valid(&hostile));
    require(gsh_functions_snapshot_payload_size(&hostile) == 0);
    require(!gsh_functions_snapshot_finalize(
        &workspace->function_scratch, &hostile));
    return true;
}

static bool fuzz_function_store(const uint8_t *data, size_t size,
                                const gsh_parse_storage *storage)
{
    if (data == NULL || storage == NULL) {
        return false;
    }
    fuzz_workspace *workspace = fuzz_storage();
    gsh_function_store *functions;
    gsh_function_store *scratch;
    gsh_function_store *snapshot;
    gsh_function_snapshot_header header;
    size_t index;
    size_t offset;
    size_t total;

    require(data != NULL || size == 0);
    require(storage != NULL);
    require(workspace != NULL);
    functions = &workspace->functions;
    scratch = &workspace->function_scratch;
    snapshot = &workspace->function_snapshot;
    gsh_functions_initialize(functions);
    gsh_functions_initialize(scratch);
    for (index = 0; index < storage->node_count; index++) {
        if (storage->nodes[index].kind == GSH_AST_FUNCTION) {
            (void)gsh_functions_set(
                functions, scratch,
                (const char *)data, size, storage, index, 0);
        }
    }
    gsh_functions_snapshot_header(functions, size, &header);
    require(gsh_functions_snapshot_header_valid(&header));
    require(header.base_generation == size);
    total = gsh_functions_snapshot_payload_size(&header);
    gsh_functions_initialize(snapshot);
    offset = 0;
    while (offset < total) {
        size_t source_available;
        size_t destination_available;
        const void *source = gsh_functions_snapshot_source(
            functions, &header, offset, &source_available);
        void *destination = gsh_functions_snapshot_destination(
            snapshot, &header, offset,
            &destination_available);
        size_t amount = source_available < destination_available
                            ? source_available
                            : destination_available;

        require(source != NULL && destination != NULL && amount > 0);
        (void)memcpy(destination, source, amount);
        offset += amount;
    }
    require(gsh_functions_snapshot_source(
                functions, &header, total, &offset) == NULL);
    require(offset == 0);
    require(gsh_functions_snapshot_finalize(
        snapshot, &header));
    require(gsh_functions_count(snapshot) ==
            gsh_functions_count(functions));
    for (index = 0; index < functions->count; index++) {
        const gsh_function_entry *entry = &functions->entries[index];
        const char *name = gsh_functions_text(functions) +
                           entry->source_offset;

        require(gsh_functions_lookup(snapshot, name,
                                     entry->name_length) != NULL);
    }
    require(gsh_functions_clone(scratch, functions));
    if (total > 0 && size > 0) {
        size_t available;
        size_t mutation = (size_t)data[0] % total;
        unsigned char *destination = gsh_functions_snapshot_destination(
            scratch, &header, mutation, &available);

        require(destination != NULL && available > 0);
        *destination ^= size > 1 ? data[1] : UINT8_C(0xff);
        (void)gsh_functions_snapshot_finalize(scratch, &header);
    }
    if (size > 0 && !fuzz_hostile_function_header(data, size, &header)) {
        return false;
    }
    return true;
}

static bool fuzz_lexer_determinism(const uint8_t *data, size_t size)
{
    if (data == NULL) {
        return false;
    }
    gsh_lexer first;
    gsh_lexer second;
    size_t steps = 0;

    require(data != NULL || size == 0);
    require(size <= 1024U * 1024U);
    gsh_lexer_init(&first, data, size);
    gsh_lexer_init(&second, data, size);
    for (steps = 0; steps <= 1024U * 1024U + 1U; steps++) {
        gsh_token left;
        gsh_token right;
        size_t previous = first.offset;
        gsh_lex_status left_status = gsh_lexer_next(&first, &left);
        gsh_lex_status right_status = gsh_lexer_next(&second, &right);

        require(left_status == right_status);
        require(first.offset == second.offset);
        require(first.status == second.status);
        require(first.error_offset == second.error_offset);
        require(first.offset <= size);
        require(first.line >= 1 && first.column >= 1);
        if (left_status != GSH_LEX_OK) {
            require(first.error_offset <= size);
            break;
        }
        require(left.kind == right.kind);
        require(left.begin == right.begin && left.end == right.end);
        require(left.line == right.line && left.column == right.column);
        require(left.begin >= previous);
        require(left.end >= left.begin && left.end <= size);
        require(left.line >= 1 && left.column >= 1);
        require(strcmp(gsh_token_kind_name(left.kind), "INVALID") != 0);
        if (left.kind == GSH_TOKEN_EOF) {
            require(first.offset == size);
            require(left.begin == size && left.end == size);
            break;
        }
        require(first.offset > previous);
        require(left.end > left.begin);
        require(steps <= size);
    }
    return true;
}

static bool fuzz_planned_command(const gsh_native_pipeline *pipeline,
                                 const gsh_native_command *command)
{
    if (command == NULL || pipeline == NULL) {
        return false;
    }
    size_t argument;
    size_t redirect;

    require(pipeline != NULL && command != NULL);
    require(command->argc > 0 || command->assignment_count > 0 ||
            command->expansion_error);
    if (command->expansion_error) {
        require(command->argc == 0);
        require(command->assignment_count == 0);
    }
    require(command->argc <= GSH_NATIVE_ARGUMENT_CAP);
    require(command->assignment_count <= GSH_NATIVE_ASSIGNMENT_CAP);
    require(command->redirect_count <= GSH_NATIVE_REDIRECT_CAP);
    require(command->argv[command->argc] == NULL);
    for (argument = 0; argument < command->argc; argument++) {
        size_t offset;

        require(pointer_offset(command->argv[argument], pipeline->text,
                               pipeline->text_used, false, &offset));
        require(memchr(pipeline->text + offset, '\0',
                       pipeline->text_used - offset) != NULL);
    }
    for (argument = 0; argument < command->assignment_count; argument++) {
        size_t offset;
        size_t remaining;

        require(pointer_offset(command->assignments[argument],
                               pipeline->text, pipeline->text_used,
                               false, &offset));
        remaining = pipeline->text_used - offset;
        require(memchr(pipeline->text + offset, '=', remaining) != NULL);
        require(memchr(pipeline->text + offset, '\0', remaining) != NULL);
    }
    for (redirect = 0; redirect < command->redirect_count; redirect++) {
        const gsh_native_redirect *item = &command->redirects[redirect];
        size_t offset;

        if (item->operator_kind == GSH_TOKEN_DLESS ||
            item->operator_kind == GSH_TOKEN_DLESSDASH) {
            require(item->heredoc_index < pipeline->heredoc_count);
        } else {
            require(pointer_offset(item->target, pipeline->text,
                                   pipeline->text_used, false, &offset));
        }
    }
    return true;
}

static bool fuzz_planned_pipeline(const gsh_native_pipeline *pipeline)
{
    if (pipeline == NULL) {
        return false;
    }
    size_t index;

    require(pipeline != NULL && pipeline->command_count > 0);
    require(pipeline->command_count <= GSH_NATIVE_PIPELINE_CAP);
    require(pipeline->text_used <= GSH_NATIVE_TEXT_CAP);
    require(pipeline->heredoc_count <= GSH_NATIVE_HEREDOC_CAP);
    require(pipeline->heredoc_text_used <= GSH_NATIVE_HEREDOC_TEXT_CAP);
    for (index = 0; index < pipeline->command_count; index++) {
        require(fuzz_planned_command(pipeline,
                                     &pipeline->commands[index]));
    }
    for (index = 0; index < pipeline->heredoc_count; index++) {
        const gsh_native_heredoc *heredoc = &pipeline->heredocs[index];
        size_t offset;

        require(pointer_offset(heredoc->body, pipeline->heredoc_text,
                               pipeline->heredoc_text_used, true, &offset));
        require(heredoc->length <= pipeline->heredoc_text_used - offset);
    }
    return true;
}

static bool fuzz_for_expansions(
    const uint8_t *data, const gsh_parse_storage *storage,
    const gsh_native_expansion_context *context,
    gsh_native_pipeline *pipeline)
{
    if (context == NULL || data == NULL || pipeline == NULL || storage == NULL) {
        return false;
    }
    size_t index;

    require(data != NULL && storage != NULL);
    require(context != NULL && pipeline != NULL);
    for (index = 0; index < storage->node_count; index++) {
        const gsh_ast_node *node = &storage->nodes[index];

        if (node->kind == GSH_AST_FOR && node->word_count > 0 &&
            (node->flags & GSH_AST_FLAG_FOR_HAS_IN) != 0) {
            char *items[GSH_NATIVE_ARGUMENT_CAP];
            size_t count;
            size_t item;
            gsh_native_plan_status status = gsh_native_expand_words(
                (const char *)data,
                storage->words + node->first_word + 1U,
                node->word_count - 1U, context, pipeline, items, &count);

            if (status != GSH_NATIVE_PLAN_OK) continue;
            require(count <= GSH_NATIVE_ARGUMENT_CAP);
            for (item = 0; item < count; item++) {
                size_t offset;

                require(pointer_offset(items[item], pipeline->text,
                                       pipeline->text_used, false, &offset));
            }
        }
    }
    return true;
}

static bool fuzz_parse_tree(const gsh_parse_storage *storage, size_t size)
{
    if (storage == NULL) {
        return false;
    }
    size_t index;

    require(storage != NULL);
    require(storage->node_count <= GSH_PARSE_NODE_CAP);
    for (index = 0; index < storage->node_count; index++) {
        const gsh_ast_node *node = &storage->nodes[index];
        size_t child = node->first_child;
        size_t children = 0;

        require(node->begin <= node->end && node->end <= size);
        require(node->first_word + node->word_count <= storage->word_count);
        require(node->first_redirect + node->redirect_count <=
                storage->redirect_count);
        while (child != GSH_AST_NONE) {
            require(child < storage->node_count);
            child = storage->nodes[child].next_sibling;
            require(++children <= storage->node_count);
        }
    }
    for (index = 0; index < storage->word_count; index++) {
        require(storage->words[index].begin <= storage->words[index].end);
        require(storage->words[index].end <= size);
    }
    for (index = 0; index < storage->redirect_count; index++) {
        const gsh_redirect *item = &storage->redirects[index];

        require(item->target.begin <= item->target.end);
        require(item->target.end <= size);
        require(item->body.begin <= item->body.end);
        require(item->body.end <= size);
    }
    for (index = 0; index < storage->heredoc_count; index++) {
        const gsh_heredoc *item = &storage->heredocs[index];

        require(item->delimiter.begin <= item->delimiter.end);
        require(item->delimiter.end <= size);
        require(item->body.begin <= item->body.end);
        require(item->body.end <= size);
    }
    return true;
}

static bool fuzz_parse_and_plan(const uint8_t *data, size_t size)
{
    if (data == NULL) {
        return false;
    }
    fuzz_workspace *workspace = fuzz_storage();
    gsh_parse_storage *storage;
    gsh_native_pipeline *pipeline;
    gsh_parse_result result;
    char *environment[] = {(char *)"IFS= \t\n", (char *)"VALUE=fuzz", NULL};
    char *positionals[] = {(char *)"one two", (char *)"", (char *)"three"};
    gsh_native_substitution_state substitutions;
    gsh_native_expansion_context context = {
        .last_status = 42, .shell_pid = 12345,
        .last_background_pid = size > 1U && (data[1] & 1U) != 0 ? 6789 : 0,
        .parameter_zero = "fuzz-zero", .positional_parameters = positionals,
        .positional_count = sizeof(positionals) / sizeof(positionals[0]),
        .option_flags = "i", .environment = environment,
        .substitutions = &substitutions,
        .pathname_mode = GSH_NATIVE_PATHNAME_PREFLIGHT,
        .nounset = size > 0 && (data[0] & 1U) != 0,
    };

    require(workspace != NULL);
    storage = &workspace->parse_storage;
    pipeline = &workspace->native_pipeline;
    result = gsh_parse(data, size, storage);
    require(storage->token_count <= GSH_PARSE_TOKEN_CAP);
    require(storage->node_count <= GSH_PARSE_NODE_CAP);
    require(storage->word_count <= GSH_PARSE_WORD_CAP);
    require(storage->redirect_count <= GSH_PARSE_REDIRECT_CAP);
    require(storage->heredoc_count <= GSH_PARSE_REDIRECT_CAP);
    require(result.error_offset <= size);
    gsh_native_substitutions_initialize(&substitutions, false);
    if (result.status == GSH_PARSE_OK) {
        gsh_native_plan_status planned;

        require(result.root < storage->node_count);
        require(fuzz_function_store(data, size, storage));
        planned = gsh_native_plan_pipeline_with_context(
            (const char *)data, storage, result.root, &context, pipeline);
        if (planned == GSH_NATIVE_PLAN_OK) {
            require(fuzz_planned_pipeline(pipeline));
        }
        require(fuzz_for_expansions(data, storage, &context, pipeline));
    }
    require(fuzz_parse_tree(storage, size));
    return true;
}

static bool fuzz_alias_parse(const uint8_t *data, size_t size)
{
    if (data == NULL) {
        return false;
    }
    fuzz_workspace *workspace = fuzz_storage();
    gsh_parse_storage *storage;
    gsh_alias_store *aliases;
    const char *parsed_input;
    size_t parsed_length;
    gsh_parse_result result;
    size_t index;

    require(data != NULL || size == 0);
    require(size <= 1024U * 1024U);
    require(workspace != NULL);
    storage = &workspace->parse_storage;
    aliases = &workspace->aliases;
    gsh_aliases_initialize(aliases);
    require(gsh_aliases_set(aliases, "a", 1, "b", 1) == 0);
    require(gsh_aliases_set(aliases, "b", 1, "a", 1) == 0);
    require(gsh_aliases_set(aliases, "p", 1, "a ", 2) == 0);
    require(gsh_aliases_set(aliases, "say", 3,
                            "/bin/echo", 9) == 0);
    result = gsh_alias_parse(
        (const char *)data, size, aliases, workspace->alias_expansion,
        sizeof(workspace->alias_expansion), storage, &parsed_input,
        &parsed_length);
    require(result.error_offset <=
            (size > GSH_ALIAS_EXPANSION_CAP ? size : GSH_ALIAS_EXPANSION_CAP));
    require(parsed_input == (const char *)data ||
            parsed_input == workspace->alias_expansion);
    require(parsed_length == size ||
            parsed_input == workspace->alias_expansion);
    require(parsed_length < GSH_ALIAS_EXPANSION_CAP ||
            result.status == GSH_PARSE_LIMIT);
    if (result.status != GSH_PARSE_OK) return true;
    require(result.root < storage->node_count);
    for (index = 0; index < storage->word_count; index++) {
        require(storage->words[index].begin <= storage->words[index].end);
        require(storage->words[index].end <= parsed_length);
    }
    return true;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (data == NULL) {
        return -1;
    }
    if (size > 1024U * 1024U) return 0;
    if (!fuzz_lexer_determinism(data, size) ||
        !fuzz_parse_and_plan(data, size) ||
        !fuzz_alias_parse(data, size)) return -1;
    return 0;
}

#ifdef GSH_FUZZ_STANDALONE
static bool known_case(const char *input, const gsh_token_kind *expected,
                       size_t expected_count, gsh_lex_status final_status)
{
    if (input == NULL || (expected == NULL && expected_count != 0U)) {
        return false;
    }
    gsh_lexer lexer;
    size_t index;

    gsh_lexer_init(&lexer, input, strlen(input));
    for (index = 0; index < expected_count; index++) {
        gsh_token token;

        require(gsh_lexer_next(&lexer, &token) == GSH_LEX_OK);
        require(token.kind == expected[index]);
    }
    {
        gsh_token token;
        gsh_lex_status status = gsh_lexer_next(&lexer, &token);

        require(status == final_status);
        if (final_status == GSH_LEX_OK) {
            require(token.kind == GSH_TOKEN_EOF);
        }
    }
    return true;
}

static uint64_t random_next(uint64_t *state)
{
    if (state == NULL) {
        return 0U;
    }
    uint64_t value = *state;

    value ^= value << 13;
    value ^= value >> 7;
    value ^= value << 17;
    *state = value;
    return value;
}

static bool known_parse(const char *input, gsh_parse_status expected)
{
    if (input == NULL) {
        return false;
    }
    static gsh_parse_storage storage;
    gsh_parse_result result = gsh_parse(input, strlen(input), &storage);

    require(result.status == expected);
    if (expected == GSH_PARSE_OK) {
        require(result.root < storage.node_count);
        require(storage.nodes[result.root].kind == GSH_AST_PROGRAM);
    }
    return true;
}

static bool known_deep_parse(void)
{
    enum { DEPTH = 400, COMMAND_CAP = DEPTH * 5 + 4 };
    char command[COMMAND_CAP];
    size_t index = 0U;
    size_t depth;

    for (depth = 0U; depth < DEPTH; depth++) {
        require(index + 2U < sizeof(command));
        command[index++] = '{';
        command[index++] = ' ';
    }
    command[index++] = ':';
    for (depth = 0U; depth < DEPTH; depth++) {
        require(index + 3U < sizeof(command));
        command[index++] = ';';
        command[index++] = ' ';
        command[index++] = '}';
    }
    command[index] = '\0';
    return known_parse(command, GSH_PARSE_OK);
}

static bool poisoned_regression(const uint8_t *input, size_t length,
                                unsigned char poison)
{
    if (input == NULL) {
        return false;
    }
    fuzz_workspace *workspace = fuzz_storage();

    require(workspace != NULL);
    (void)memset(&workspace->parse_storage, poison,
           sizeof(workspace->parse_storage));
    (void)memset(&workspace->native_pipeline, (unsigned char)~poison,
           sizeof(workspace->native_pipeline));
    return LLVMFuzzerTestOneInput(input, length) == 0;
}

static int parse_fuzz_case_count(const char *text, unsigned long *cases)
{
    char *end;

    if (cases == NULL) return -1;
    if (text == NULL) return 0;
    *cases = strtoul(text, &end, 10);
    if (*end == '\0' && *cases != 0) return 0;
    (void)fprintf(stderr, "usage: lexer-fuzz-smoke [positive-cases]\n");
    return -1;
}

static bool random_fuzz_cases(unsigned long cases)
{
    uint64_t state = UINT64_C(0x6a09e667f3bcc909);
    uint8_t buffer[4096];
    unsigned long iteration;
    bool passed = true;

    for (iteration = 0; iteration < cases; iteration++) {
        size_t length = (size_t)(random_next(&state) % sizeof(buffer));
        size_t index;

        for (index = 0; index < length; index++) {
            buffer[index] = (uint8_t)random_next(&state);
        }
        passed &= LLVMFuzzerTestOneInput(buffer, length) == 0;
    }
    return passed;
}

int main(int argc, char **argv)
{
    if (argv == NULL) {
        return -1;
    }
    static const uint8_t heredoc_arithmetic_regression[] = {
        0x25, 0x62, 0x69, 0x7c, 0x74, 0x20, 0x3c, 0x3c, 0x45, 0x4f,
        0x46, 0x0a, 0x76, 0x61, 0x6c, 0xff, 0x75, 0xff, 0x3c, 0x61,
        0x24, 0x28, 0x28, 0x31, 0x20, 0x20, 0x3c, 0x3c, 0x45, 0x4f,
        0x46, 0x0a, 0x76, 0x3c, 0x3c, 0x20, 0x32, 0x29, 0x29, 0x20,
        0x24, 0x28, 0x28, 0x31, 0x20, 0x3c, 0x3c, 0x32, 0x09, 0x20,
        0x29, 0x29, 0x0a, 0xff, 0x0a, 0x45, 0x4f, 0x46, 0x0a,
    };
    static const uint8_t pointer_range_regression[] = {
        0x2f, 0x65, 0x7c, 0x74, 0x20, 0x3c, 0x3c, 0x45, 0x4f, 0x46,
        0x0a, 0x76, 0x61, 0x6c, 0x75, 0x65, 0x24, 0x74, 0x46, 0x3c,
        0x3e, 0x56, 0xf7, 0xff, 0xff, 0xff, 0x29, 0x22, 0x55, 0x26,
        0x0a, 0x45, 0x4f, 0x29, 0x22, 0x55, 0x26, 0x0a, 0x45, 0x4f,
        0x46, 0x5c, 0x0a,
    };
    static const uint8_t control_byte_regression[] = {
        0x2f, 0x62, 0x69, 0x6e, 0x7c, 0x63, 0x61, 0x74, 0x20, 0x3c,
        0x3c, 0x45, 0x4f, 0x46, 0x0a, 0x76, 0x61, 0x6c, 0x75, 0x65,
        0x20, 0x24, 0x56, 0x41, 0x4c, 0x08, 0x55, 0x45, 0x0a, 0x45,
        0x4f, 0x46, 0x0a,
    };
    static const uint8_t arithmetic_assignment_regression[] =
        ": \"$((GSH_FUZZ_ARITH=7,"
        "GSH_FUZZ_ARITH+=010,0&&(GSH_FUZZ_ARITH=0)))\"";
    static const uint8_t parameter_assignment_regression[] =
        "qalue${out=eQr<<-(printf )}\n";
    static const gsh_token_kind basic[] = {
        GSH_TOKEN_WORD, GSH_TOKEN_WORD, GSH_TOKEN_AND_IF,
        GSH_TOKEN_WORD, GSH_TOKEN_NEWLINE,
    };
    static const gsh_token_kind comment[] = {
        GSH_TOKEN_NEWLINE, GSH_TOKEN_WORD, GSH_TOKEN_CLOBBER,
        GSH_TOKEN_WORD,
    };
    static const gsh_token_kind one_word[] = {GSH_TOKEN_WORD};
    unsigned long cases = 100000;
    bool passed = true;

    if (parse_fuzz_case_count(argc > 1 ? argv[1] : NULL, &cases) == -1) {
        return 2;
    }
    passed &= known_case("echo 'a b' && x\n", basic,
               sizeof(basic) / sizeof(basic[0]), GSH_LEX_OK);
    passed &= known_case("# comment\nx>|y", comment,
               sizeof(comment) / sizeof(comment[0]), GSH_LEX_OK);
    passed &= known_case("x=$(printf '%s' \")\")", one_word, 1, GSH_LEX_OK);
    passed &= known_case("x=$((1 + (2 * 3)))", one_word, 1, GSH_LEX_OK);
    passed &= known_case("x=${value:-\"a b\"}", one_word, 1, GSH_LEX_OK);
    passed &= known_case("x=${value##\"a*\"}", one_word, 1, GSH_LEX_OK);
    passed &= known_case("x=${value#?*?*[[:digit:]]*z}", one_word, 1,
               GSH_LEX_OK);
    passed &= known_case("'unterminated", NULL, 0, GSH_LEX_INCOMPLETE);
    passed &= known_parse("a && b || c; d &\n", GSH_PARSE_OK);
    passed &= known_parse("(a; b) | c", GSH_PARSE_OK);
    passed &= known_parse("{ a; b; }", GSH_PARSE_OK);
    passed &= known_parse("if a; then b; elif c; then d; else e; fi",
                GSH_PARSE_OK);
    passed &= known_parse("while a; do b; done", GSH_PARSE_OK);
    passed &= known_parse("until a; do b; done", GSH_PARSE_OK);
    passed &= known_parse("for i in a b; do echo $i; done", GSH_PARSE_OK);
    passed &= known_parse("f() { echo x; } >out", GSH_PARSE_OK);
    passed &= known_parse("case x in x) :;; esac", GSH_PARSE_OK);
    passed &= known_parse("case x in (a|b) : ;& x) :;; esac", GSH_PARSE_OK);
    passed &= known_parse("cat <<EOF\ntext\nEOF\n", GSH_PARSE_OK);
    passed &= known_parse("cat <<'EOF'\ntext\nEOF\n", GSH_PARSE_OK);
    passed &= known_parse("cat <<-EOF\n\ttext\n\tEOF\n", GSH_PARSE_OK);
    passed &= known_parse("cat <<EOF\nfoo\\\nEOF\nbar\nEOF\n", GSH_PARSE_OK);
    passed &= known_parse("cat <<EOF\ntext\n", GSH_PARSE_INCOMPLETE);
    passed &= known_parse("if true; then", GSH_PARSE_INCOMPLETE);
    passed &= known_deep_parse();
    passed &= poisoned_regression(heredoc_arithmetic_regression,
                        sizeof(heredoc_arithmetic_regression), 0xa5U);
    passed &= poisoned_regression(pointer_range_regression,
                        sizeof(pointer_range_regression), 0x5aU);
    passed &= poisoned_regression(control_byte_regression,
                        sizeof(control_byte_regression), 0x3cU);
    passed &= poisoned_regression(arithmetic_assignment_regression,
                        sizeof(arithmetic_assignment_regression) - 1U,
                        0xc3U);
    passed &= poisoned_regression(parameter_assignment_regression,
                        sizeof(parameter_assignment_regression) - 1U,
                        0x96U);

    passed &= random_fuzz_cases(cases);
    if (!passed) return 1;
    (void)printf("lexer/parser/planner/alias/function fuzz smoke: cases=%lu "
           "seed=0x%llx passed\n",
           cases,
           (unsigned long long)UINT64_C(0x6a09e667f3bcc909));
    return 0;
}
#endif
