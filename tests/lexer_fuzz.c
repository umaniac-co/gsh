#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif
#if _POSIX_C_SOURCE < 202405L
#error "gsh fuzzers require the POSIX.1-2024 feature-test baseline"
#endif

#include "../src/posix_lexer.h"
#include "../src/posix_parser.h"
#include "../src/native_plan.h"
#include "../src/alias_expansion.h"
#include "../src/shell_aliases.h"
#include "../src/shell_functions.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void require_at(int condition, int line)
{
    if (!condition) {
        fprintf(stderr, "fuzz invariant failed at line %d\n", line);
        abort();
    }
}

#define require(condition) require_at((condition), __LINE__)

static bool pointer_offset(const void *pointer, const void *base,
                           size_t length, bool allow_end, size_t *offset)
{
    uintptr_t address = (uintptr_t)pointer;
    uintptr_t begin = (uintptr_t)base;

    if (address < begin || address - begin > length ||
        (!allow_end && address - begin == length)) {
        return false;
    }
    *offset = (size_t)(address - begin);
    return true;
}

static gsh_parse_storage fuzz_parse_storage;
static gsh_native_pipeline fuzz_native_pipeline;
static gsh_alias_store fuzz_aliases;
static char fuzz_alias_expansion[GSH_ALIAS_EXPANSION_CAP];
static gsh_function_store fuzz_functions;
static gsh_function_store fuzz_function_scratch;
static gsh_function_store fuzz_function_snapshot;

static void fuzz_function_store(const uint8_t *data, size_t size,
                                const gsh_parse_storage *storage)
{
    gsh_function_snapshot_header header;
    size_t index;
    size_t offset;
    size_t total;

    gsh_functions_initialize(&fuzz_functions);
    gsh_functions_initialize(&fuzz_function_scratch);
    for (index = 0; index < storage->node_count; index++) {
        if (storage->nodes[index].kind == GSH_AST_FUNCTION) {
            (void)gsh_functions_set(
                &fuzz_functions, &fuzz_function_scratch,
                (const char *)data, size, storage, index, 0);
        }
    }
    gsh_functions_snapshot_header(&fuzz_functions, size, &header);
    require(gsh_functions_snapshot_header_valid(&header));
    require(header.base_generation == size);
    total = gsh_functions_snapshot_payload_size(&header);
    gsh_functions_initialize(&fuzz_function_snapshot);
    offset = 0;
    while (offset < total) {
        size_t source_available;
        size_t destination_available;
        const void *source = gsh_functions_snapshot_source(
            &fuzz_functions, &header, offset, &source_available);
        void *destination = gsh_functions_snapshot_destination(
            &fuzz_function_snapshot, &header, offset,
            &destination_available);
        size_t amount = source_available < destination_available
                            ? source_available
                            : destination_available;

        require(source != NULL && destination != NULL && amount > 0);
        memcpy(destination, source, amount);
        offset += amount;
    }
    require(gsh_functions_snapshot_source(
                &fuzz_functions, &header, total, &offset) == NULL);
    require(offset == 0);
    require(gsh_functions_snapshot_finalize(
        &fuzz_function_snapshot, &header));
    require(gsh_functions_count(&fuzz_function_snapshot) ==
            gsh_functions_count(&fuzz_functions));
    for (index = 0; index < fuzz_functions.count; index++) {
        const gsh_function_entry *entry = &fuzz_functions.entries[index];
        const char *name = gsh_functions_text(&fuzz_functions) +
                           entry->source_offset;

        require(gsh_functions_lookup(&fuzz_function_snapshot, name,
                                     entry->name_length) != NULL);
    }
    require(gsh_functions_clone(&fuzz_function_scratch,
                                &fuzz_functions));
    if (total > 0 && size > 0) {
        size_t available;
        size_t mutation = (size_t)data[0] % total;
        unsigned char *destination = gsh_functions_snapshot_destination(
            &fuzz_function_scratch, &header, mutation, &available);

        require(destination != NULL && available > 0);
        *destination ^= size > 1 ? data[1] : UINT8_C(0xff);
        (void)gsh_functions_snapshot_finalize(&fuzz_function_scratch,
                                              &header);
    }
    if (size > 0) {
        gsh_function_snapshot_header hostile = header;

        switch (data[size - 1U] % 7U) {
        case 0:
            hostile.version++;
            break;
        case 1:
            hostile.count = UINT32_MAX;
            break;
        case 2:
            hostile.text_used = UINT32_MAX;
            break;
        case 3:
            hostile.node_count = UINT32_MAX;
            break;
        case 4:
            hostile.word_count = UINT32_MAX;
            break;
        case 5:
            hostile.redirect_count = UINT32_MAX;
            break;
        default:
            hostile.reserved = 1;
            break;
        }
        require(!gsh_functions_snapshot_header_valid(&hostile));
        require(gsh_functions_snapshot_payload_size(&hostile) == 0);
        require(!gsh_functions_snapshot_finalize(
            &fuzz_function_scratch, &hostile));
    }
}

static gsh_native_plan_status fuzz_command_substitution(
    void *opaque, const char *commands, size_t command_length, char *output,
    size_t output_capacity, size_t *output_length, int *exit_status)
{
    (void)opaque;
    (void)commands;
    *output_length = 0;
    *exit_status = 0;
    if ((command_length & 1U) != 0) {
        if (output_capacity == 0) {
            return GSH_NATIVE_PLAN_LIMIT;
        }
        output[0] = 'x';
        *output_length = 1;
    }
    return GSH_NATIVE_PLAN_OK;
}

static gsh_native_plan_status fuzz_parameter_error(
    void *opaque, const char *name, size_t name_length,
    const char *message, size_t message_length, bool default_message)
{
    (void)opaque;
    (void)name;
    (void)name_length;
    (void)message;
    (void)message_length;
    (void)default_message;
    return GSH_NATIVE_PLAN_ERROR;
}

static gsh_native_plan_status fuzz_expansion_error(
    void *opaque, const char *message, size_t message_length)
{
    (void)opaque;
    require(message != NULL && message_length > 0);
    return GSH_NATIVE_PLAN_ERROR;
}

static gsh_native_plan_status fuzz_variable_assign(
    void *opaque, const char *name, size_t name_length, const char *value,
    size_t value_length)
{
    size_t offset;
    volatile unsigned char observed = 0;

    (void)opaque;
    require(name != NULL && name_length > 0 && value != NULL);
    for (offset = 0; offset < name_length; offset++) {
        require((name[offset] >= 'a' && name[offset] <= 'z') ||
                (name[offset] >= 'A' && name[offset] <= 'Z') ||
                name[offset] == '_' ||
                (offset > 0 && name[offset] >= '0' && name[offset] <= '9'));
    }
    require(value_length <= GSH_NATIVE_TEXT_CAP);
    for (offset = 0; offset < value_length; offset++) {
        observed ^= (unsigned char)value[offset];
    }
    (void)observed;
    return GSH_NATIVE_PLAN_OK;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    gsh_lexer first;
    gsh_lexer second;
    size_t steps = 0;

    if (size > 1024U * 1024U) {
        return 0;
    }
    gsh_lexer_init(&first, data, size);
    gsh_lexer_init(&second, data, size);
    for (;;) {
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
        steps++;
        require(steps <= size + 1U);
    }
    {
        gsh_parse_storage *parse_storage = &fuzz_parse_storage;
        gsh_native_pipeline *native_pipeline = &fuzz_native_pipeline;
        gsh_parse_result result = gsh_parse(data, size, parse_storage);
        size_t index;

        require(parse_storage->token_count <= GSH_PARSE_TOKEN_CAP);
        require(parse_storage->node_count <= GSH_PARSE_NODE_CAP);
        require(parse_storage->word_count <= GSH_PARSE_WORD_CAP);
        require(parse_storage->redirect_count <= GSH_PARSE_REDIRECT_CAP);
        require(parse_storage->heredoc_count <= GSH_PARSE_REDIRECT_CAP);
        require(result.error_offset <= size);
        if (result.status == GSH_PARSE_OK) {
            gsh_native_plan_status planned;
            char *environment[] = {(char *)"IFS= \t\n",
                                   (char *)"VALUE=fuzz", NULL};
            char *positionals[] = {(char *)"one two", (char *)"",
                                   (char *)"three"};
            const gsh_native_expansion_context expansion = {
                .last_status = 42,
                .shell_pid = 12345,
                .last_background_pid =
                    size > 1U && (data[1] & 1U) != 0 ? 6789 : 0,
                .parameter_zero = "fuzz-zero",
                .positional_parameters = positionals,
                .positional_count =
                    sizeof(positionals) / sizeof(positionals[0]),
                .option_flags = "i",
                .environment = environment,
                .variable_assign = fuzz_variable_assign,
                .parameter_error = fuzz_parameter_error,
                .expansion_error = fuzz_expansion_error,
                .command_substitute = fuzz_command_substitution,
                .pathname_mode = GSH_NATIVE_PATHNAME_PREFLIGHT,
                .nounset = size > 0 && (data[0] & 1U) != 0,
            };

            require(result.root < parse_storage->node_count);
            fuzz_function_store(data, size, parse_storage);
            planned = gsh_native_plan_pipeline_with_context(
                (const char *)data, parse_storage, result.root,
                &expansion, native_pipeline);
            if (planned == GSH_NATIVE_PLAN_OK) {
                require(native_pipeline->command_count > 0);
                require(native_pipeline->command_count <=
                        GSH_NATIVE_PIPELINE_CAP);
                require(native_pipeline->text_used <= GSH_NATIVE_TEXT_CAP);
                require(native_pipeline->heredoc_count <=
                        GSH_NATIVE_HEREDOC_CAP);
                require(native_pipeline->heredoc_text_used <=
                        GSH_NATIVE_HEREDOC_TEXT_CAP);
                for (index = 0; index < native_pipeline->command_count;
                     index++) {
                    const gsh_native_command *command =
                        &native_pipeline->commands[index];
                    size_t argument;
                    size_t redirect;

                    require(command->argc > 0 ||
                            command->assignment_count > 0 ||
                            command->expansion_error);
                    if (command->expansion_error) {
                        require(command->argc == 0);
                        require(command->assignment_count == 0);
                    }
                    require(command->argc <= GSH_NATIVE_ARGUMENT_CAP);
                    require(command->assignment_count <=
                            GSH_NATIVE_ASSIGNMENT_CAP);
                    require(command->redirect_count <=
                            GSH_NATIVE_REDIRECT_CAP);
                    require(command->argv[command->argc] == NULL);
                    for (argument = 0; argument < command->argc;
                         argument++) {
                        size_t text_offset;

                        require(pointer_offset(
                            command->argv[argument], native_pipeline->text,
                            native_pipeline->text_used, false,
                            &text_offset));
                        require(memchr(native_pipeline->text + text_offset,
                                      '\0', native_pipeline->text_used -
                                                text_offset) != NULL);
                    }
                    for (argument = 0;
                         argument < command->assignment_count; argument++) {
                        size_t text_offset;
                        size_t remaining;

                        require(pointer_offset(
                            command->assignments[argument],
                            native_pipeline->text,
                            native_pipeline->text_used, false,
                            &text_offset));
                        remaining = native_pipeline->text_used - text_offset;
                        require(memchr(native_pipeline->text + text_offset,
                                      '=', remaining) != NULL);
                        require(memchr(native_pipeline->text + text_offset,
                                      '\0', remaining) != NULL);
                    }
                    for (redirect = 0;
                         redirect < command->redirect_count; redirect++) {
                        const gsh_native_redirect *planned_redirect =
                            &command->redirects[redirect];

                        if (planned_redirect->operator_kind ==
                                GSH_TOKEN_DLESS ||
                            planned_redirect->operator_kind ==
                                GSH_TOKEN_DLESSDASH) {
                            require(planned_redirect->heredoc_index <
                                    native_pipeline->heredoc_count);
                        } else {
                            size_t text_offset;

                            require(pointer_offset(
                                planned_redirect->target,
                                native_pipeline->text,
                                native_pipeline->text_used, false,
                                &text_offset));
                        }
                    }
                }
                for (index = 0; index < native_pipeline->heredoc_count;
                     index++) {
                    const gsh_native_heredoc *heredoc =
                        &native_pipeline->heredocs[index];
                    size_t text_offset;

                    require(pointer_offset(
                        heredoc->body, native_pipeline->heredoc_text,
                        native_pipeline->heredoc_text_used, true,
                        &text_offset));
                    require(heredoc->length <=
                            native_pipeline->heredoc_text_used -
                                text_offset);
                }
            }
            for (index = 0; index < parse_storage->node_count; index++) {
                const gsh_ast_node *node = &parse_storage->nodes[index];

                if (node->kind == GSH_AST_FOR && node->word_count > 0 &&
                    (node->flags & GSH_AST_FLAG_FOR_HAS_IN) != 0) {
                    char *items[GSH_NATIVE_ARGUMENT_CAP];
                    size_t item_count;
                    gsh_native_plan_status expanded =
                        gsh_native_expand_words(
                            (const char *)data,
                            parse_storage->words + node->first_word + 1U,
                            node->word_count - 1U, &expansion,
                            native_pipeline, items, &item_count);

                    if (expanded == GSH_NATIVE_PLAN_OK) {
                        size_t item;

                        require(item_count <= GSH_NATIVE_ARGUMENT_CAP);
                        for (item = 0; item < item_count; item++) {
                            size_t text_offset;

                            require(pointer_offset(
                                items[item], native_pipeline->text,
                                native_pipeline->text_used, false,
                                &text_offset));
                        }
                    }
                }
            }
        }
        for (index = 0; index < parse_storage->node_count; index++) {
            const gsh_ast_node *node = &parse_storage->nodes[index];
            size_t child = node->first_child;
            size_t children = 0;

            require(node->begin <= node->end && node->end <= size);
            require(node->first_word + node->word_count <=
                    parse_storage->word_count);
            require(node->first_redirect + node->redirect_count <=
                    parse_storage->redirect_count);
            while (child != GSH_AST_NONE) {
                require(child < parse_storage->node_count);
                child = parse_storage->nodes[child].next_sibling;
                children++;
                require(children <= parse_storage->node_count);
            }
        }
        for (index = 0; index < parse_storage->word_count; index++) {
            require(parse_storage->words[index].begin <=
                    parse_storage->words[index].end);
            require(parse_storage->words[index].end <= size);
        }
        for (index = 0; index < parse_storage->redirect_count; index++) {
            require(parse_storage->redirects[index].target.begin <=
                    parse_storage->redirects[index].target.end);
            require(parse_storage->redirects[index].target.end <= size);
            require(parse_storage->redirects[index].body.begin <=
                    parse_storage->redirects[index].body.end);
            require(parse_storage->redirects[index].body.end <= size);
        }
        for (index = 0; index < parse_storage->heredoc_count; index++) {
            require(parse_storage->heredocs[index].delimiter.begin <=
                    parse_storage->heredocs[index].delimiter.end);
            require(parse_storage->heredocs[index].delimiter.end <= size);
            require(parse_storage->heredocs[index].body.begin <=
                    parse_storage->heredocs[index].body.end);
            require(parse_storage->heredocs[index].body.end <= size);
        }
    }
    {
        const char *parsed_input;
        size_t parsed_length;
        gsh_parse_result result;
        size_t index;

        gsh_aliases_initialize(&fuzz_aliases);
        require(gsh_aliases_set(&fuzz_aliases, "a", 1, "b", 1) == 0);
        require(gsh_aliases_set(&fuzz_aliases, "b", 1, "a", 1) == 0);
        require(gsh_aliases_set(&fuzz_aliases, "p", 1, "a ", 2) == 0);
        require(gsh_aliases_set(&fuzz_aliases, "say", 3,
                                "/bin/echo", 9) == 0);
        result = gsh_alias_parse(
            (const char *)data, size, &fuzz_aliases,
            fuzz_alias_expansion, sizeof(fuzz_alias_expansion),
            &fuzz_parse_storage, &parsed_input, &parsed_length);
        require(result.status != GSH_PARSE_REWRITE);
        require(result.error_offset <=
                (size > GSH_ALIAS_EXPANSION_CAP
                     ? size
                     : GSH_ALIAS_EXPANSION_CAP));
        require(parsed_input == (const char *)data ||
                parsed_input == fuzz_alias_expansion);
        require(parsed_length == size ||
                parsed_input == fuzz_alias_expansion);
        require(parsed_length < GSH_ALIAS_EXPANSION_CAP ||
                result.status == GSH_PARSE_LIMIT);
        if (result.status == GSH_PARSE_OK) {
            require(result.root < fuzz_parse_storage.node_count);
            for (index = 0; index < fuzz_parse_storage.word_count;
                 index++) {
                require(fuzz_parse_storage.words[index].begin <=
                        fuzz_parse_storage.words[index].end);
                require(fuzz_parse_storage.words[index].end <=
                        parsed_length);
            }
        }
    }
    return 0;
}

#ifdef GSH_FUZZ_STANDALONE
static void known_case(const char *input, const gsh_token_kind *expected,
                       size_t expected_count, gsh_lex_status final_status)
{
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
}

static uint64_t random_next(uint64_t *state)
{
    uint64_t value = *state;

    value ^= value << 13;
    value ^= value >> 7;
    value ^= value << 17;
    *state = value;
    return value;
}

static void known_parse(const char *input, gsh_parse_status expected)
{
    static gsh_parse_storage storage;
    gsh_parse_result result = gsh_parse(input, strlen(input), &storage);

    require(result.status == expected);
    if (expected == GSH_PARSE_OK) {
        require(result.root < storage.node_count);
        require(storage.nodes[result.root].kind == GSH_AST_PROGRAM);
    }
}

static void poisoned_regression(const uint8_t *input, size_t length,
                                unsigned char poison)
{
    memset(&fuzz_parse_storage, poison, sizeof(fuzz_parse_storage));
    memset(&fuzz_native_pipeline, (unsigned char)~poison,
           sizeof(fuzz_native_pipeline));
    (void)LLVMFuzzerTestOneInput(input, length);
}

int main(int argc, char **argv)
{
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
    uint64_t random_state = UINT64_C(0x6a09e667f3bcc909);
    uint8_t buffer[4096];
    unsigned long iteration;

    if (argc > 1) {
        char *end;

        cases = strtoul(argv[1], &end, 10);
        if (*end != '\0' || cases == 0) {
            fprintf(stderr, "usage: lexer-fuzz-smoke [positive-cases]\n");
            return 2;
        }
    }
    known_case("echo 'a b' && x\n", basic,
               sizeof(basic) / sizeof(basic[0]), GSH_LEX_OK);
    known_case("# comment\nx>|y", comment,
               sizeof(comment) / sizeof(comment[0]), GSH_LEX_OK);
    known_case("x=$(printf '%s' \")\")", one_word, 1, GSH_LEX_OK);
    known_case("x=$((1 + (2 * 3)))", one_word, 1, GSH_LEX_OK);
    known_case("x=${value:-\"a b\"}", one_word, 1, GSH_LEX_OK);
    known_case("x=${value##\"a*\"}", one_word, 1, GSH_LEX_OK);
    known_case("x=${value#?*?*[[:digit:]]*z}", one_word, 1,
               GSH_LEX_OK);
    known_case("'unterminated", NULL, 0, GSH_LEX_INCOMPLETE);
    known_parse("a && b || c; d &\n", GSH_PARSE_OK);
    known_parse("(a; b) | c", GSH_PARSE_OK);
    known_parse("{ a; b; }", GSH_PARSE_OK);
    known_parse("if a; then b; elif c; then d; else e; fi",
                GSH_PARSE_OK);
    known_parse("while a; do b; done", GSH_PARSE_OK);
    known_parse("until a; do b; done", GSH_PARSE_OK);
    known_parse("for i in a b; do echo $i; done", GSH_PARSE_OK);
    known_parse("f() { echo x; } >out", GSH_PARSE_OK);
    known_parse("case x in x) :;; esac", GSH_PARSE_OK);
    known_parse("case x in (a|b) : ;& x) :;; esac", GSH_PARSE_OK);
    known_parse("cat <<EOF\ntext\nEOF\n", GSH_PARSE_OK);
    known_parse("cat <<'EOF'\ntext\nEOF\n", GSH_PARSE_OK);
    known_parse("cat <<-EOF\n\ttext\n\tEOF\n", GSH_PARSE_OK);
    known_parse("cat <<EOF\nfoo\\\nEOF\nbar\nEOF\n", GSH_PARSE_OK);
    known_parse("cat <<EOF\ntext\n", GSH_PARSE_INCOMPLETE);
    known_parse("if true; then", GSH_PARSE_INCOMPLETE);
    poisoned_regression(heredoc_arithmetic_regression,
                        sizeof(heredoc_arithmetic_regression), 0xa5U);
    poisoned_regression(pointer_range_regression,
                        sizeof(pointer_range_regression), 0x5aU);
    poisoned_regression(control_byte_regression,
                        sizeof(control_byte_regression), 0x3cU);
    poisoned_regression(arithmetic_assignment_regression,
                        sizeof(arithmetic_assignment_regression) - 1U,
                        0xc3U);
    poisoned_regression(parameter_assignment_regression,
                        sizeof(parameter_assignment_regression) - 1U,
                        0x96U);

    for (iteration = 0; iteration < cases; iteration++) {
        size_t length = (size_t)(random_next(&random_state) % sizeof(buffer));
        size_t index;

        for (index = 0; index < length; index++) {
            buffer[index] = (uint8_t)random_next(&random_state);
        }
        (void)LLVMFuzzerTestOneInput(buffer, length);
    }
    printf("lexer/parser/planner/alias/function fuzz smoke: cases=%lu "
           "seed=0x%llx passed\n",
           cases,
           (unsigned long long)UINT64_C(0x6a09e667f3bcc909));
    return 0;
}
#endif
