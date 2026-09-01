CC ?= cc
CPPFLAGS ?=
CFLAGS ?= -O2
CFLAGS += -std=c17 -Wall -Wextra -Wpedantic -Werror
LDFLAGS ?=

TARGET := build/gsh
HISTORY_AGENT_TARGET := build/gsh-history-agent
SOURCES := src/gsh.c src/async_repl.c src/posix_lexer.c \
	src/posix_parser.c src/native_plan.c \
	src/source_workspace.c \
	src/history_client.c src/history_store.c src/shell_config.c \
	src/shell_variables.c src/builtin_common.c src/command_cache.c \
	src/builtin_command.c \
	src/builtin_cd.c src/builtin_times.c src/builtin_ulimit.c \
	src/builtin_umask.c src/builtin_variables.c src/shell_aliases.c \
	src/alias_expansion.c src/builtin_alias.c src/builtin_unalias.c \
	src/shell_functions.c \
	src/positional_parameters.c src/shell_options.c src/builtin_set.c \
	src/builtin_shift.c src/background_jobs.c
TEST_TARGET := build/pty-harness
TEST_SOURCE := tests/pty_smoke.c
PROBE_TARGET := build/job-probe
PROBE_SOURCE := tests/job_probe.c
FAULT_TARGET := build/gsh-fault
SANITIZE_TARGET := build/gsh-sanitize
FUZZ_SMOKE_TARGET := build/lexer-fuzz-smoke
FUZZ_TARGET := build/lexer-fuzz
POLICY_TARGET := build/source-policy
CONFORMANCE_TARGET := build/posix-conformance
VARIABLE_TEST_TARGET := build/shell-variables-test
COMMAND_CACHE_TEST_TARGET := build/command-cache-test
COMMAND_CACHE_SANITIZE_TEST_TARGET := build/command-cache-test-sanitize
POSITIONAL_TEST_TARGET := build/positional-parameters-test
BACKGROUND_TEST_TARGET := build/background-jobs-test
ALIAS_TEST_TARGET := build/shell-aliases-test
FUNCTION_TEST_TARGET := build/shell-functions-test
FUNCTION_SANITIZE_TEST_TARGET := build/shell-functions-test-sanitize
CONFIG_TEST_TARGET := build/shell-config-test
SOURCE_WORKSPACE_TEST_TARGET := build/source-workspace-test
BASH_BIN ?= $(shell command -v bash)
ZSH_BIN ?= $(shell command -v zsh)
SOAK_SECONDS ?= 60
FUZZ_CASES ?= 1000000
FUZZ_SECONDS ?= 60
PTY_FUZZ_CASES ?= 2000
FUZZ_CC ?= clang
ANALYZE_CC ?= clang
FUZZ_SEED_CORPUS ?= tests/corpus/lexer
FUZZ_WORK_CORPUS ?= dev/fuzz/lexer
PERFORMANCE_DIR ?= dev/performance
BENCH_OUTPUT ?= $(PERFORMANCE_DIR)/current.raw.txt
ALIAS_BENCH_OUTPUT ?= $(PERFORMANCE_DIR)/current-alias.raw.txt
SODIUM_PREFIX := $(shell if command -v brew >/dev/null 2>&1; then \
	brew --prefix libsodium 2>/dev/null; fi)
SODIUM_CFLAGS := $(shell if command -v pkg-config >/dev/null 2>&1; then \
	pkg-config --cflags libsodium 2>/dev/null; elif test -n "$(SODIUM_PREFIX)"; \
	then echo -I$(SODIUM_PREFIX)/include; fi)
SODIUM_LIBS := $(shell if command -v pkg-config >/dev/null 2>&1; then \
	pkg-config --libs libsodium 2>/dev/null; elif test -n "$(SODIUM_PREFIX)"; \
	then echo -L$(SODIUM_PREFIX)/lib -lsodium; else echo -lsodium; fi)

.PHONY: all analyze bench bench-record bench-alias bench-alias-record check check-fault check-resource check-sanitize clean
.PHONY: check-aliases check-background check-command-cache check-config check-functions check-positionals check-source-workspaces check-variables conformance fuzz-libfuzzer fuzz-pty fuzz-pty-sanitize fuzz-sanitize
.PHONY: fuzz-smoke policy soak
.PHONY: verify-fast

all: $(TARGET) $(HISTORY_AGENT_TARGET)

$(TARGET): $(SOURCES) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SOURCES) $(LDFLAGS) -o $@

$(HISTORY_AGENT_TARGET): src/history_agent.c src/history_store.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SODIUM_CFLAGS) \
		src/history_agent.c src/history_store.c $(LDFLAGS) \
		$(SODIUM_LIBS) -o $@

$(TEST_TARGET): $(TEST_SOURCE) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(TEST_SOURCE) $(LDFLAGS) -o $@

$(PROBE_TARGET): $(PROBE_SOURCE) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(PROBE_SOURCE) $(LDFLAGS) -o $@

$(FAULT_TARGET): $(SOURCES) | build
	$(CC) $(CPPFLAGS) -DGSH_FAULT_INJECTION $(CFLAGS) $(SOURCES) $(LDFLAGS) -o $@

$(SANITIZE_TARGET): $(SOURCES) | build
	clang $(CPPFLAGS) -O1 -g -std=c17 -Wall -Wextra -Wpedantic -Werror \
		-fsanitize=address,undefined $(SOURCES) $(LDFLAGS) -o $@

$(FUZZ_SMOKE_TARGET): tests/lexer_fuzz.c src/posix_lexer.c src/posix_parser.c src/native_plan.c src/shell_aliases.c src/alias_expansion.c src/shell_functions.c | build
	$(CC) $(CPPFLAGS) -DGSH_FUZZ_STANDALONE $(CFLAGS) \
		tests/lexer_fuzz.c src/posix_lexer.c src/posix_parser.c \
		src/native_plan.c src/shell_aliases.c src/alias_expansion.c \
		src/shell_functions.c \
		$(LDFLAGS) -o $@

$(FUZZ_TARGET): tests/lexer_fuzz.c src/posix_lexer.c src/posix_parser.c src/native_plan.c src/shell_aliases.c src/alias_expansion.c src/shell_functions.c | build
	$(FUZZ_CC) $(CPPFLAGS) -O1 -g -std=c17 -Wall -Wextra -Wpedantic \
		-Werror -fsanitize=fuzzer,address,undefined \
		tests/lexer_fuzz.c src/posix_lexer.c src/posix_parser.c \
		src/native_plan.c src/shell_aliases.c src/alias_expansion.c \
		src/shell_functions.c \
		$(LDFLAGS) -o $@

$(POLICY_TARGET): tests/source_policy.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/source_policy.c $(LDFLAGS) -o $@

$(CONFORMANCE_TARGET): tests/posix_conformance.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/posix_conformance.c $(LDFLAGS) -o $@

$(VARIABLE_TEST_TARGET): tests/shell_variables_test.c src/shell_variables.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/shell_variables_test.c \
	src/shell_variables.c $(LDFLAGS) -o $@

$(COMMAND_CACHE_TEST_TARGET): tests/command_cache_test.c \
		src/command_cache.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/command_cache_test.c \
		src/command_cache.c $(LDFLAGS) -o $@

$(COMMAND_CACHE_SANITIZE_TEST_TARGET): tests/command_cache_test.c \
		src/command_cache.c | build
	clang $(CPPFLAGS) -O1 -g -std=c17 -Wall -Wextra -Wpedantic \
		-Werror -fsanitize=address,undefined \
		tests/command_cache_test.c src/command_cache.c $(LDFLAGS) -o $@

$(POSITIONAL_TEST_TARGET): tests/positional_parameters_test.c src/positional_parameters.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/positional_parameters_test.c \
		src/positional_parameters.c $(LDFLAGS) -o $@

$(BACKGROUND_TEST_TARGET): tests/background_jobs_test.c src/background_jobs.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/background_jobs_test.c \
	src/background_jobs.c $(LDFLAGS) -o $@

$(ALIAS_TEST_TARGET): tests/shell_aliases_test.c src/shell_aliases.c \
		src/alias_expansion.c src/posix_lexer.c src/posix_parser.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/shell_aliases_test.c \
		src/shell_aliases.c src/alias_expansion.c src/posix_lexer.c \
		src/posix_parser.c $(LDFLAGS) -o $@

$(FUNCTION_TEST_TARGET): tests/shell_functions_test.c src/shell_functions.c \
		src/posix_lexer.c src/posix_parser.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/shell_functions_test.c \
		src/shell_functions.c src/posix_lexer.c src/posix_parser.c \
		$(LDFLAGS) -o $@

$(FUNCTION_SANITIZE_TEST_TARGET): tests/shell_functions_test.c \
		src/shell_functions.c src/posix_lexer.c src/posix_parser.c | build
	clang $(CPPFLAGS) -O1 -g -std=c17 -Wall -Wextra -Wpedantic \
		-Werror -fsanitize=address,undefined \
		tests/shell_functions_test.c src/shell_functions.c \
		src/posix_lexer.c src/posix_parser.c $(LDFLAGS) -o $@

$(CONFIG_TEST_TARGET): tests/shell_config_test.c src/shell_config.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/shell_config_test.c \
		src/shell_config.c $(LDFLAGS) -o $@

$(SOURCE_WORKSPACE_TEST_TARGET): tests/source_workspace_test.c \
		src/source_workspace.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/source_workspace_test.c \
		src/source_workspace.c $(LDFLAGS) -o $@

check: $(TARGET) $(HISTORY_AGENT_TARGET) $(TEST_TARGET) $(PROBE_TARGET)
	./$(TEST_TARGET) $(abspath $(TARGET))

check-fault: $(FAULT_TARGET) $(TEST_TARGET)
	./$(TEST_TARGET) --fault $(abspath $(FAULT_TARGET))

check-resource: $(TARGET) $(TEST_TARGET)
	./$(TEST_TARGET) --resource $(abspath $(TARGET))

check-sanitize: $(SANITIZE_TARGET) $(HISTORY_AGENT_TARGET) \
		$(FUNCTION_SANITIZE_TEST_TARGET) \
		$(COMMAND_CACHE_SANITIZE_TEST_TARGET) \
		$(TEST_TARGET) $(PROBE_TARGET)
	ASAN_OPTIONS=abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
		./$(TEST_TARGET) $(abspath $(SANITIZE_TARGET))
	ASAN_OPTIONS=abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
		./$(FUNCTION_SANITIZE_TEST_TARGET)
	ASAN_OPTIONS=abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
		./$(COMMAND_CACHE_SANITIZE_TEST_TARGET)

fuzz-smoke: $(FUZZ_SMOKE_TARGET)
	./$(FUZZ_SMOKE_TARGET) $(FUZZ_CASES)

fuzz-sanitize: | build
	clang $(CPPFLAGS) -DGSH_FUZZ_STANDALONE -O1 -g -std=c17 \
		-Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined \
		tests/lexer_fuzz.c src/posix_lexer.c src/posix_parser.c \
		src/native_plan.c src/shell_aliases.c src/alias_expansion.c \
		src/shell_functions.c \
		-o $(FUZZ_SMOKE_TARGET)-sanitize
	ASAN_OPTIONS=abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
		./$(FUZZ_SMOKE_TARGET)-sanitize $(FUZZ_CASES)

fuzz-libfuzzer: $(FUZZ_TARGET) | $(FUZZ_WORK_CORPUS)
	ASAN_OPTIONS=abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
		./$(FUZZ_TARGET) $(FUZZ_WORK_CORPUS) $(FUZZ_SEED_CORPUS) \
		-max_total_time=$(FUZZ_SECONDS) \
		-print_final_stats=1

$(FUZZ_WORK_CORPUS):
	mkdir -p $@

fuzz-pty: $(TARGET) $(TEST_TARGET) $(PROBE_TARGET)
	./$(TEST_TARGET) --fuzz-pty $(abspath $(TARGET)) $(PTY_FUZZ_CASES)

fuzz-pty-sanitize: $(SANITIZE_TARGET) $(TEST_TARGET) $(PROBE_TARGET)
	ASAN_OPTIONS=abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
		./$(TEST_TARGET) --fuzz-pty $(abspath $(SANITIZE_TARGET)) \
		$(PTY_FUZZ_CASES)

policy: $(TARGET) $(POLICY_TARGET)
	./$(POLICY_TARGET) $(CURDIR) $(abspath $(TARGET))

analyze:
	@set -e; for source in $(SOURCES); do \
		$(ANALYZE_CC) $(CPPFLAGS) $(CFLAGS) --analyze "$$source" \
			-o /dev/null; \
	done
	$(ANALYZE_CC) $(CPPFLAGS) $(CFLAGS) $(SODIUM_CFLAGS) --analyze \
		src/history_agent.c -o /dev/null

conformance: $(TARGET) $(CONFORMANCE_TARGET)
	./$(CONFORMANCE_TARGET) $(abspath $(TARGET))

check-variables: $(VARIABLE_TEST_TARGET)
	./$(VARIABLE_TEST_TARGET)

check-command-cache: $(COMMAND_CACHE_TEST_TARGET)
	./$(COMMAND_CACHE_TEST_TARGET)

check-positionals: $(POSITIONAL_TEST_TARGET)
	./$(POSITIONAL_TEST_TARGET)

check-background: $(BACKGROUND_TEST_TARGET)
	./$(BACKGROUND_TEST_TARGET)

check-aliases: $(ALIAS_TEST_TARGET)
	./$(ALIAS_TEST_TARGET)

check-functions: $(FUNCTION_TEST_TARGET)
	./$(FUNCTION_TEST_TARGET)

check-config: $(CONFIG_TEST_TARGET)
	./$(CONFIG_TEST_TARGET)

check-source-workspaces: $(SOURCE_WORKSPACE_TEST_TARGET)
	./$(SOURCE_WORKSPACE_TEST_TARGET)

soak: $(TARGET) $(TEST_TARGET)
	./$(TEST_TARGET) --soak $(abspath $(TARGET)) $(SOAK_SECONDS)

verify-fast:
	$(MAKE) -j1 policy
	$(MAKE) -j1 analyze
	$(MAKE) -j1 conformance
	$(MAKE) -j1 check
	$(MAKE) -j1 check-variables
	$(MAKE) -j1 check-command-cache
	$(MAKE) -j1 check-positionals
	$(MAKE) -j1 check-background
	$(MAKE) -j1 check-config
	$(MAKE) -j1 check-source-workspaces
	$(MAKE) -j1 check-aliases
	$(MAKE) -j1 check-functions
	$(MAKE) -j1 check-fault
	$(MAKE) -j1 check-resource
	$(MAKE) -j1 fuzz-smoke
	$(MAKE) -j1 fuzz-sanitize
	$(MAKE) -j1 fuzz-pty
	$(MAKE) -j1 check-sanitize

bench: $(TARGET) $(TEST_TARGET)
	@$(CC) --version | sed -n '1{s/^/compiler: /;p;}'
	@$(BASH_BIN) --version | sed -n '1{s/^/bash-version: /;p;}'
	@$(ZSH_BIN) --version | sed -n '1{s/^/zsh-version: /;p;}'
	./$(TEST_TARGET) --benchmark $(abspath $(TARGET)) $(BASH_BIN) $(ZSH_BIN)

bench-record: $(TARGET) $(TEST_TARGET) | $(PERFORMANCE_DIR)
	$(MAKE) --no-print-directory bench > $(BENCH_OUTPUT)

bench-alias: $(TARGET) $(TEST_TARGET)
	@$(CC) --version | sed -n '1{s/^/compiler: /;p;}'
	@$(BASH_BIN) --version | sed -n '1{s/^/bash-version: /;p;}'
	@$(ZSH_BIN) --version | sed -n '1{s/^/zsh-version: /;p;}'
	./$(TEST_TARGET) --benchmark-alias $(abspath $(TARGET)) $(BASH_BIN) $(ZSH_BIN)

bench-alias-record: $(TARGET) $(TEST_TARGET) | $(PERFORMANCE_DIR)
	$(MAKE) --no-print-directory bench-alias > $(ALIAS_BENCH_OUTPUT)

$(PERFORMANCE_DIR):
	mkdir -p $@

build:
	mkdir -p $@

clean:
	rm -f $(TARGET) $(HISTORY_AGENT_TARGET) $(TEST_TARGET) $(PROBE_TARGET) $(FAULT_TARGET)
	rm -f $(SANITIZE_TARGET) $(FUZZ_SMOKE_TARGET) $(FUZZ_TARGET) $(POLICY_TARGET)
	rm -f $(CONFORMANCE_TARGET)
	rm -f $(VARIABLE_TEST_TARGET)
	rm -f $(COMMAND_CACHE_TEST_TARGET)
	rm -f $(COMMAND_CACHE_SANITIZE_TEST_TARGET)
	rm -f $(POSITIONAL_TEST_TARGET)
	rm -f $(BACKGROUND_TEST_TARGET)
	rm -f $(CONFIG_TEST_TARGET)
	rm -f $(SOURCE_WORKSPACE_TEST_TARGET)
	rm -f $(ALIAS_TEST_TARGET)
	rm -f $(FUNCTION_TEST_TARGET)
	rm -f $(FUNCTION_SANITIZE_TEST_TARGET)
	rm -f build/posix_parser.o build/pty-smoke
	rm -f $(FUZZ_SMOKE_TARGET)-sanitize
	rm -rf $(TARGET).dSYM $(TEST_TARGET).dSYM $(PROBE_TARGET).dSYM
	rm -rf build/gsh-sanitize.dSYM
	rmdir build 2>/dev/null || true
