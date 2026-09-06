#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif

#include "llm_command_policy.h"

#include <stdio.h>
#include <string.h>

/* ── Approval Is Reserved For Removal Commands ───────────────────
 * The old read-only allowlist interrupted navigation, builds and ordinary
 * writes. The policy now recognizes removal operations in parsed commands;
 * all other commands run automatically, including unfamiliar tools. Parsing
 * distinguishes executable words from printed text, comments and heredocs.
 * Literal shell wrappers use a bounded worklist rather than recursive scans.
 * This recognizes CLI syntax; it does not evaluate arbitrary program effects.
 * ─────────────────────────────────────────────────────────────── */
static bool listed(const char *word, const char *list)
{
    char surrounded[128];
    int length;

    if (word == NULL || list == NULL) return false;
    length = snprintf(surrounded, sizeof(surrounded), "|%s|", word);
    return length > 0 && (size_t)length < sizeof(surrounded) &&
        strstr(list, surrounded) != NULL;
}

static bool read_word(const char *script, const gsh_parse_storage *parse,
                      const gsh_ast_node *node, size_t index,
                      char word[GSH_LLM_POLICY_SCRIPT_CAP])
{
    gsh_word_ref ref;
    size_t offset;
    size_t used = 0U;
    char quote = '\0';

    if (script == NULL || parse == NULL || node == NULL || word == NULL ||
        index >= node->word_count ||
        node->first_word + index >= parse->word_count) return false;
    ref = parse->words[node->first_word + index];
    if (ref.end >= GSH_LLM_POLICY_SCRIPT_CAP || ref.begin > ref.end)
        return false;
    for (offset = ref.begin; offset < ref.end; offset++) {
        char byte = script[offset];
        if (byte == '\\' && quote != '\'' && offset + 1U < ref.end) {
            char next = script[offset + 1U];
            if (quote != '"' || strchr("$`\"\\\n", next) != NULL) {
                offset++;
                if (next != '\n') word[used++] = next;
                continue;
            }
        }
        if (byte == quote) { quote = '\0'; continue; }
        if (quote == '\0' && (byte == '\'' || byte == '"')) {
            quote = byte;
            continue;
        }
        word[used++] = byte;
    }
    word[used] = '\0';
    return true;
}

static const char *command_basename(const char *word)
{
    const char *slash;
    if (word == NULL) return "";
    slash = strrchr(word, '/');
    return slash == NULL ? word : slash + 1U;
}

static bool wrapper(const char *command)
{
    if (command == NULL) return false;
    return listed(command, "|command|exec|env|sudo|doas|nohup|nice|time|xargs|");
}

static bool wrapper_value(const char *command, const char *word)
{
    if (command == NULL || word == NULL) return false;
    if (strcmp(command, "env") == 0) return listed(word, "|-u|--unset|-C|--chdir|");
    if (strcmp(command, "sudo") == 0)
        return listed(word, "|-u|-g|-h|-p|-C|-T|-R|-D|--user|--group|");
    if (strcmp(command, "doas") == 0) return listed(word, "|-u|-C|");
    if (strcmp(command, "nice") == 0) return listed(word, "|-n|--adjustment|");
    if (strcmp(command, "xargs") == 0)
        return listed(word, "|-I|-J|-L|-n|-P|-s|-E|");
    return false;
}

static bool informational(const char *word)
{
    if (word == NULL) return false;
    return listed(word, "|--help|--version|");
}

static bool find_value(const char *word)
{
    if (word == NULL) return false;
    return listed(word, "|-name|-iname|-path|-ipath|-wholename|-regex|-iregex|"
        "-type|-xtype|-user|-group|-uid|-gid|-size|-perm|-mtime|-atime|-ctime|"
        "-mmin|-amin|-cmin|-maxdepth|-mindepth|-newer|-printf|-fprintf|-fprint|");
}

static bool find_deletes(const char *script, const gsh_parse_storage *parse,
                         const gsh_ast_node *node, size_t begin)
{
    char word[GSH_LLM_POLICY_SCRIPT_CAP];
    bool skip = false;
    bool execution = false;
    size_t index;

    if (script == NULL || parse == NULL || node == NULL) return false;
    for (index = begin; index < node->word_count; index++) {
        if (!read_word(script, parse, node, index, word)) return false;
        if (skip) { skip = false; continue; }
        if (execution) {
            if (listed(word, "|;|+|")) execution = false;
            continue;
        }
        if (find_value(word)) skip = true;
        else if (listed(word, "|-exec|-execdir|-ok|-okdir|")) execution = true;
        else if (strcmp(word, "-delete") == 0) return true;
    }
    return false;
}

static bool removal_arguments(const char *script,
    const gsh_parse_storage *parse, const gsh_ast_node *node,
    size_t begin, const char *command)
{
    char word[GSH_LLM_POLICY_SCRIPT_CAP];
    bool removal = false;
    bool operand = false;
    bool options = true;
    bool dry_run = false;
    size_t index;

    if (script == NULL || parse == NULL || node == NULL || command == NULL)
        return false;
    for (index = begin; index < node->word_count; index++) {
        if (!read_word(script, parse, node, index, word)) return false;
        if (options && informational(word)) return false;
        if (options && strcmp(word, "--") == 0) { options = false; continue; }
        if (!options || word[0] != '-') operand = true;
        if (options && (strcmp(word, "--dry-run") == 0 ||
            (word[0] == '-' && word[1] != '-' && strchr(word, 'n') != NULL)))
            dry_run = true;
        if (listed(command, "|branch|tag|stash|") && options &&
            (strcmp(word, "--delete") == 0 ||
             (word[0] == '-' && word[1] != '-' &&
              (strchr(word, 'd') != NULL || strchr(word, 'D') != NULL))))
            removal = true;
        if (strcmp(command, "stash") == 0 && listed(word, "|drop|clear|"))
            removal = true;
    }
    if (listed(command, "|branch|tag|stash|")) return removal;
    if (strcmp(command, "clean") == 0) return !dry_run;
    if (listed(command, "|git-rm|delete|remove|uninstall|purge|prune|"))
        return !dry_run && (operand || strcmp(command, "prune") == 0);
    return operand;
}

static void enqueue_script(gsh_llm_command_policy *policy, const char *script)
{
    size_t length;
    if (policy == NULL || script == NULL) return;
    length = strlen(script);
    if (length == 0U || length >= GSH_LLM_POLICY_SCRIPT_CAP ||
        policy->count >= GSH_LLM_POLICY_NESTING_CAP) return;
    (void)memcpy(policy->scripts[policy->count++], script, length + 1U);
}

static size_t substitution_end(const char *text, size_t length)
{
    gsh_lexer lexer;
    gsh_token token;
    size_t depth = 1U;
    size_t steps;

    if (text == NULL || length == 0U) return SIZE_MAX;
    gsh_lexer_init(&lexer, text, length);
    for (steps = 0U; steps < GSH_PARSE_TOKEN_CAP; steps++) {
        if (gsh_lexer_next(&lexer, &token) != GSH_LEX_OK ||
            token.kind == GSH_TOKEN_EOF) return SIZE_MAX;
        if (token.kind == GSH_TOKEN_LPAREN) depth++;
        if (token.kind == GSH_TOKEN_RPAREN && --depth == 0U) return token.begin;
    }
    return SIZE_MAX;
}

static void enqueue_substitutions(const char *script, gsh_word_ref ref,
                                  gsh_llm_command_policy *policy)
{
    char nested[GSH_LLM_POLICY_SCRIPT_CAP];
    char quote = '\0';
    size_t offset;

    if (script == NULL || policy == NULL || ref.begin > ref.end ||
        ref.end >= GSH_LLM_POLICY_SCRIPT_CAP) return;
    for (offset = ref.begin; offset < ref.end; offset++) {
        char byte = script[offset];
        size_t begin;
        size_t length;
        if (byte == '\\' && quote != '\'') { offset++; continue; }
        if (byte == quote) { quote = '\0'; continue; }
        if (quote == '\'') continue;
        if (quote == '\0' && (byte == '\'' || byte == '"')) {
            quote = byte;
            continue;
        }
        if (byte == '$' && offset + 2U < ref.end &&
            script[offset + 1U] == '(' && script[offset + 2U] != '(') {
            begin = offset + 2U;
            length = substitution_end(script + begin, ref.end - begin);
            if (length == SIZE_MAX) continue;
        } else if (byte == '`') {
            begin = offset + 1U;
            for (length = 0U; begin + length < ref.end; length++) {
                if (script[begin + length] == '\\') length++;
                else if (script[begin + length] == '`') break;
            }
            if (begin + length >= ref.end) continue;
        } else continue;
        (void)memcpy(nested, script + begin, length);
        nested[length] = '\0';
        enqueue_script(policy, nested);
        offset = begin + length;
    }
}

static void enqueue_eval(const char *script, const gsh_ast_node *node,
                          size_t begin, gsh_llm_command_policy *policy)
{
    char nested[GSH_LLM_POLICY_SCRIPT_CAP];
    char word[GSH_LLM_POLICY_SCRIPT_CAP];
    size_t used = 0U;
    size_t index;

    if (script == NULL || node == NULL || policy == NULL) return;
    for (index = begin; index < node->word_count; index++) {
        size_t length;
        if (!read_word(script, &policy->parse, node, index, word)) return;
        length = strlen(word);
        if (length + 1U >= sizeof(nested) - used) return;
        if (used != 0U) nested[used++] = ' ';
        (void)memcpy(nested + used, word, length);
        used += length;
    }
    nested[used] = '\0';
    enqueue_script(policy, nested);
}

static bool cli_removes(const char *script, const gsh_ast_node *node,
                        size_t begin, const char *command,
                        const gsh_llm_command_policy *policy)
{
    char word[GSH_LLM_POLICY_SCRIPT_CAP];
    bool skip = false;
    bool git;
    size_t index;

    if (script == NULL || node == NULL || command == NULL || policy == NULL)
        return false;
    git = strcmp(command, "git") == 0;
    for (index = begin; index < node->word_count; index++) {
        if (!read_word(script, &policy->parse, node, index, word)) return false;
        if (skip) { skip = false; continue; }
        if (listed(word, git ? "|-C|-c|--git-dir|--work-tree|"
                             : "|-n|--namespace|--context|-H|--host|--kubeconfig|")) {
            skip = true;
            continue;
        }
        if (word[0] == '-') continue;
        if (listed(word, git ? "|rm|clean|branch|tag|stash|"
                             : "|rm|rmi|delete|remove|uninstall|purge|prune|"))
            return removal_arguments(script, &policy->parse, node,
                index + 1U, git && strcmp(word, "rm") == 0 ? "git-rm" : word);
        if (git || !listed(word, "|container|image|volume|network|system|"))
            return false;
    }
    return false;
}

static bool simple_removes(const char *script, const gsh_ast_node *node,
                           gsh_llm_command_policy *policy)
{
    char word[GSH_LLM_POLICY_SCRIPT_CAP];
    char command[128] = {0};
    size_t index;
    bool skip = false;

    if (script == NULL || node == NULL || policy == NULL) return false;
    for (index = 0U; index < node->word_count; index++) {
        const char *name;
        if (!read_word(script, &policy->parse, node, index, word)) return false;
        if (skip) { skip = false; continue; }
        name = command_basename(word);
        if (command[0] == '\0' || wrapper(command)) {
            if (strcmp(command, "command") == 0 && listed(word, "|-v|-V|"))
                return false;
            if (word[0] == '-' || strchr(word, '=') != NULL) {
                skip = wrapper_value(command, word);
                continue;
            }
            if (strlen(name) >= sizeof(command)) return false;
            (void)memcpy(command, name, strlen(name) + 1U);
            if (listed(command, "|rm|rmdir|unlink|srm|trash|trash-put|"))
                return removal_arguments(script, &policy->parse, node,
                                           index + 1U, command);
            if (strcmp(command, "find") == 0 &&
                find_deletes(script, &policy->parse, node, index + 1U)) return true;
            continue;
        }
        if (strcmp(command, "find") == 0) {
            if (find_value(word)) skip = true;
            else if (listed(word, "|-exec|-execdir|-ok|-okdir|")) command[0] = '\0';
        } else if (strcmp(command, "git") == 0) {
            return cli_removes(script, node, index, command, policy);
        } else if (strcmp(command, "eval") == 0) {
            enqueue_eval(script, node, index, policy);
            return false;
        } else if (listed(command, "|sh|bash|zsh|dash|gsh|")) {
            if (word[0] == '-' && word[1] != '-' && strchr(word, 'c') != NULL &&
                read_word(script, &policy->parse, node, index + 1U, word)) {
                enqueue_script(policy, word);
                return false;
            }
        } else if (listed(command,
            "|docker|podman|kubectl|brew|npm|pnpm|yarn|pip|pip3|apt|apt-get|dnf|yum|")) {
            return cli_removes(script, node, index, command, policy);
        } else return false;
    }
    return false;
}

bool gsh_llm_command_removes(const char *script, gsh_llm_command_policy *policy)
{
    size_t pending;

    if (script == NULL || policy == NULL) return false;
    policy->count = 0U;
    enqueue_script(policy, script);
    for (pending = 0U; pending < policy->count; pending++) {
        const char *source = policy->scripts[pending];
        size_t index;
        if (gsh_parse(source, strlen(source), &policy->parse).status != GSH_PARSE_OK)
            continue;
        for (index = 0U; index < policy->parse.word_count; index++)
            enqueue_substitutions(source, policy->parse.words[index], policy);
        for (index = 0U; index < policy->parse.node_count; index++) {
            const gsh_ast_node *node = &policy->parse.nodes[index];
            if (node->kind == GSH_AST_SIMPLE && simple_removes(source, node, policy))
                return true;
        }
    }
    return false;
}
