CC ?= cc
CPPFLAGS ?=
CPPFLAGS += -UNDEBUG
CFLAGS ?= -O2
CFLAGS += -std=c17 -Wall -Wextra -Wpedantic -Werror \
	-ffunction-sections -fdata-sections
LDFLAGS ?=

UNAME_SYSTEM := $(shell uname -s)
ifeq ($(UNAME_SYSTEM),Darwin)
AUDIT_LDFLAGS = -Wl,-dead_strip -Wl,-map,$@.map
else
AUDIT_LDFLAGS = -Wl,--gc-sections -Wl,-Map,$@.map
endif

CC_VERSION := $(shell $(CC) --version 2>/dev/null | sed -n '1p')
ifneq ($(findstring clang,$(CC_VERSION)),)
CFLAGS += -Wunreachable-code-aggressive -Wunused-macros \
	-Wunneeded-internal-declaration -Wmissing-prototypes \
	-Wstrict-prototypes -Wmissing-variable-declarations
else
CFLAGS += -Wunreachable-code -Wunused-macros -Wmissing-prototypes \
	-Wstrict-prototypes -Wmissing-declarations
endif

BUILD_DIR ?= build
TARGET := $(BUILD_DIR)/gsh
HISTORY_AGENT_TARGET := $(BUILD_DIR)/gsh-history-agent
CORE_SOURCES := src/gsh.c src/async_repl.c src/posix_lexer.c \
	src/posix_parser.c src/native_plan.c \
	src/source_workspace.c \
	src/history_client.c src/history_store.c src/shell_config.c \
	src/shell_variables.c src/builtin_common.c src/builtin_registry.c \
	src/builtin_pure.c src/builtin_stateful.c src/builtin_fc.c \
	src/builtin_files.c \
	src/native_viewer.c \
	src/resource_actions.c \
	src/builtin_job_control.c src/command_cache.c \
	src/builtin_command.c \
	src/builtin_cd.c src/builtin_times.c src/builtin_ulimit.c \
	src/builtin_umask.c src/builtin_variables.c src/shell_aliases.c \
	src/alias_expansion.c src/builtin_alias.c src/builtin_unalias.c \
	src/shell_functions.c src/shell_traps.c src/builtin_trap.c \
	src/positional_parameters.c src/shell_options.c src/shell_invocation.c \
	src/builtin_set.c \
	src/builtin_shift.c src/background_jobs.c
SOURCES := $(CORE_SOURCES) src/fault_injection_disabled.c
FAULT_SOURCES := $(CORE_SOURCES) src/fault_injection_enabled.c
TEST_TARGET := $(BUILD_DIR)/pty-harness
TEST_SOURCES := tests/pty_smoke.c tests/benchmark_report.c
BENCHMARK_REPORT_TEST_TARGET := $(BUILD_DIR)/benchmark-report-test
PROBE_TARGET := $(BUILD_DIR)/job-probe
PROBE_SOURCE := tests/job_probe.c
FAULT_TARGET := $(BUILD_DIR)/gsh-fault
SANITIZE_TARGET := $(BUILD_DIR)/gsh-sanitize
FUZZ_SMOKE_TARGET := $(BUILD_DIR)/lexer-fuzz-smoke
FUZZ_TARGET := $(BUILD_DIR)/lexer-fuzz
POLICY_TARGET := $(BUILD_DIR)/source-policy
TARGET_MANIFEST := dev/target-manifest.tsv
CANON_C_SOURCES := $(shell awk 'NF == 3 { print $$1 }' \
	$(TARGET_MANIFEST))
CONFORMANCE_TARGET := $(BUILD_DIR)/posix-conformance
VARIABLE_TEST_TARGET := $(BUILD_DIR)/shell-variables-test
COMMAND_CACHE_TEST_TARGET := $(BUILD_DIR)/command-cache-test
COMMAND_CACHE_SANITIZE_TEST_TARGET := $(BUILD_DIR)/command-cache-test-sanitize
POSITIONAL_TEST_TARGET := $(BUILD_DIR)/positional-parameters-test
BACKGROUND_TEST_TARGET := $(BUILD_DIR)/background-jobs-test
ALIAS_TEST_TARGET := $(BUILD_DIR)/shell-aliases-test
FUNCTION_TEST_TARGET := $(BUILD_DIR)/shell-functions-test
FUNCTION_SANITIZE_TEST_TARGET := $(BUILD_DIR)/shell-functions-test-sanitize
CONFIG_TEST_TARGET := $(BUILD_DIR)/shell-config-test
FILE_BUILTINS_TEST_TARGET := $(BUILD_DIR)/file-builtins-test
FILE_BUILTINS_SANITIZE_TEST_TARGET := $(BUILD_DIR)/file-builtins-test-sanitize
RESOURCE_ACTIONS_TEST_TARGET := $(BUILD_DIR)/resource-actions-test
RESOURCE_ACTIONS_SANITIZE_TEST_TARGET := $(BUILD_DIR)/resource-actions-test-sanitize
SOURCE_WORKSPACE_TEST_TARGET := $(BUILD_DIR)/source-workspace-test
SHELL_TRAPS_TEST_TARGET := $(BUILD_DIR)/shell-traps-test
SHELL_TRAPS_SANITIZE_TEST_TARGET := $(BUILD_DIR)/shell-traps-test-sanitize
QUALITY_TARGETS := $(TARGET) $(HISTORY_AGENT_TARGET) $(TEST_TARGET) \
	$(BENCHMARK_REPORT_TEST_TARGET) $(PROBE_TARGET) $(FAULT_TARGET) \
	$(FUZZ_SMOKE_TARGET) $(POLICY_TARGET) $(CONFORMANCE_TARGET) \
	$(VARIABLE_TEST_TARGET) $(COMMAND_CACHE_TEST_TARGET) \
	$(POSITIONAL_TEST_TARGET) $(BACKGROUND_TEST_TARGET) \
	$(ALIAS_TEST_TARGET) $(FUNCTION_TEST_TARGET) $(CONFIG_TEST_TARGET) \
	$(FILE_BUILTINS_TEST_TARGET) $(RESOURCE_ACTIONS_TEST_TARGET) \
	$(SOURCE_WORKSPACE_TEST_TARGET) $(SHELL_TRAPS_TEST_TARGET)
QUALITY_MAPS := $(addsuffix .map,$(QUALITY_TARGETS))
DEPENDENCY_TARGETS := $(QUALITY_TARGETS) $(SANITIZE_TARGET) $(FUZZ_TARGET) \
	$(COMMAND_CACHE_SANITIZE_TEST_TARGET) \
	$(FUNCTION_SANITIZE_TEST_TARGET) $(SHELL_TRAPS_SANITIZE_TEST_TARGET) \
	$(FILE_BUILTINS_SANITIZE_TEST_TARGET) \
	$(RESOURCE_ACTIONS_SANITIZE_TEST_TARGET)
DEPENDENCY_FILES := $(addsuffix .d,$(DEPENDENCY_TARGETS))
BASH_BIN ?= $(shell command -v bash)
ZSH_BIN ?= $(shell command -v zsh)
SOAK_SECONDS ?= 60
FUZZ_CASES ?= 1000000
FUZZ_SECONDS ?= 60
PTY_FUZZ_CASES ?= 2000
FUZZ_CC ?= clang
ANALYZE_CC ?= clang
QUALITY_CLANG_CC ?= clang
QUALITY_GCC_CC ?= gcc
LINUX_QUALITY_IMAGE ?= gsh-quality-linux:bookworm
FUZZ_SEED_CORPUS ?= tests/corpus/lexer
FUZZ_WORK_CORPUS ?= dev/fuzz/lexer
PERFORMANCE_DIR ?= dev/performance
BENCH_OUTPUT ?= $(PERFORMANCE_DIR)/current.csv
ALIAS_BENCH_OUTPUT ?= $(PERFORMANCE_DIR)/current-alias.csv
BENCH_REVISION := $(shell revision=$$(git rev-parse --short=12 HEAD \
	2>/dev/null || echo unknown); if test -n "$$(git status --porcelain \
	--untracked-files=normal -- Makefile README.md CODE.md src tests \
	2>/dev/null)"; then printf '%s-dirty' "$$revision"; else \
	printf '%s' "$$revision"; fi)
SODIUM_PREFIX := $(shell if command -v brew >/dev/null 2>&1; then \
	brew --prefix libsodium 2>/dev/null; fi)
SODIUM_CFLAGS := $(shell if command -v pkg-config >/dev/null 2>&1; then \
	pkg-config --cflags libsodium 2>/dev/null; elif test -n "$(SODIUM_PREFIX)"; \
	then echo -I$(SODIUM_PREFIX)/include; fi)
SODIUM_LIBS := $(shell if command -v pkg-config >/dev/null 2>&1; then \
	pkg-config --libs libsodium 2>/dev/null; elif test -n "$(SODIUM_PREFIX)"; \
	then echo -L$(SODIUM_PREFIX)/lib -lsodium; else echo -lsodium; fi)

.PHONY: all analyze bench bench-record bench-alias bench-alias-record check check-benchmark-report check-fault check-resource check-sanitize clean
.PHONY: check-aliases check-background check-command-cache check-config check-file-builtins check-functions check-positionals check-resource-actions check-source-workspaces check-traps check-variables conformance fuzz-libfuzzer fuzz-pty fuzz-pty-sanitize fuzz-sanitize
.PHONY: fuzz-smoke fuzz-libfuzzer-linux policy quality quality-callgraph quality-compilers quality-dependencies quality-includes quality-linux quality-maps soak
.PHONY: verify-fast

all: $(TARGET) $(HISTORY_AGENT_TARGET)

define generate_dependencies
	@$(CC) $(CPPFLAGS) $(CFLAGS) $(2) -MM -MP -MT '$@' $(1) \
		> '$@.d.tmp'
	@mv '$@.d.tmp' '$@.d'
endef

-include $(wildcard $(DEPENDENCY_FILES))

$(TARGET) $(HISTORY_AGENT_TARGET) $(TEST_TARGET) \
	$(BENCHMARK_REPORT_TEST_TARGET) $(PROBE_TARGET) $(FAULT_TARGET) \
	$(SANITIZE_TARGET) $(FUZZ_SMOKE_TARGET) $(FUZZ_TARGET) \
	$(POLICY_TARGET) $(CONFORMANCE_TARGET) $(VARIABLE_TEST_TARGET) \
	$(COMMAND_CACHE_TEST_TARGET) $(COMMAND_CACHE_SANITIZE_TEST_TARGET) \
	$(POSITIONAL_TEST_TARGET) $(BACKGROUND_TEST_TARGET) \
	$(ALIAS_TEST_TARGET) $(FUNCTION_TEST_TARGET) \
		$(FUNCTION_SANITIZE_TEST_TARGET) $(CONFIG_TEST_TARGET) \
		$(FILE_BUILTINS_TEST_TARGET) $(RESOURCE_ACTIONS_TEST_TARGET) \
		$(FILE_BUILTINS_SANITIZE_TEST_TARGET) \
		$(RESOURCE_ACTIONS_SANITIZE_TEST_TARGET) \
		$(SOURCE_WORKSPACE_TEST_TARGET) $(SHELL_TRAPS_TEST_TARGET) \
	$(SHELL_TRAPS_SANITIZE_TEST_TARGET): Makefile

$(FILE_BUILTINS_TEST_TARGET): tests/file_builtins_test.c \
		src/builtin_files.c src/native_viewer.c src/shell_config.c \
		src/builtin_common.c src/async_repl.c src/resource_actions.c | $(BUILD_DIR)
	$(call generate_dependencies,tests/file_builtins_test.c src/builtin_files.c src/native_viewer.c src/shell_config.c src/builtin_common.c src/async_repl.c src/resource_actions.c)
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/file_builtins_test.c \
		src/builtin_files.c src/native_viewer.c src/shell_config.c \
		src/builtin_common.c src/async_repl.c src/resource_actions.c \
		$(LDFLAGS) $(AUDIT_LDFLAGS) -o $@

$(RESOURCE_ACTIONS_TEST_TARGET): tests/resource_actions_test.c \
		src/async_repl.c src/resource_actions.c | $(BUILD_DIR)
	$(call generate_dependencies,tests/resource_actions_test.c src/async_repl.c src/resource_actions.c)
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/resource_actions_test.c \
		src/async_repl.c src/resource_actions.c $(LDFLAGS) \
		$(AUDIT_LDFLAGS) -o $@

$(FILE_BUILTINS_SANITIZE_TEST_TARGET): tests/file_builtins_test.c \
		src/builtin_files.c src/native_viewer.c src/shell_config.c \
		src/builtin_common.c src/async_repl.c src/resource_actions.c | $(BUILD_DIR)
	$(call generate_dependencies,tests/file_builtins_test.c src/builtin_files.c src/native_viewer.c src/shell_config.c src/builtin_common.c src/async_repl.c src/resource_actions.c)
	clang $(CPPFLAGS) -O1 -g -std=c17 -Wall -Wextra -Wpedantic \
		-Werror -fsanitize=address,undefined tests/file_builtins_test.c \
		src/builtin_files.c src/native_viewer.c src/shell_config.c \
		src/builtin_common.c src/async_repl.c src/resource_actions.c \
		$(LDFLAGS) -o $@

$(RESOURCE_ACTIONS_SANITIZE_TEST_TARGET): tests/resource_actions_test.c \
		src/async_repl.c src/resource_actions.c | $(BUILD_DIR)
	$(call generate_dependencies,tests/resource_actions_test.c src/async_repl.c src/resource_actions.c)
	clang $(CPPFLAGS) -O1 -g -std=c17 -Wall -Wextra -Wpedantic \
		-Werror -fsanitize=address,undefined tests/resource_actions_test.c \
		src/async_repl.c src/resource_actions.c $(LDFLAGS) -o $@

$(TARGET): $(SOURCES) | $(BUILD_DIR)
	$(call generate_dependencies,$(SOURCES))
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SOURCES) $(LDFLAGS) \
		$(AUDIT_LDFLAGS) -o $@

$(HISTORY_AGENT_TARGET): src/history_agent.c src/history_store.c | $(BUILD_DIR)
	$(call generate_dependencies,src/history_agent.c src/history_store.c,$(SODIUM_CFLAGS))
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SODIUM_CFLAGS) \
		src/history_agent.c src/history_store.c $(LDFLAGS) \
		$(SODIUM_LIBS) $(AUDIT_LDFLAGS) -o $@

$(TEST_TARGET): $(TEST_SOURCES) tests/benchmark_report.h | $(BUILD_DIR)
	$(call generate_dependencies,$(TEST_SOURCES))
	$(CC) $(CPPFLAGS) $(CFLAGS) $(TEST_SOURCES) $(LDFLAGS) \
		$(AUDIT_LDFLAGS) -o $@

$(BENCHMARK_REPORT_TEST_TARGET): tests/benchmark_report_test.c \
		tests/benchmark_report.c tests/benchmark_report.h | $(BUILD_DIR)
	$(call generate_dependencies,tests/benchmark_report_test.c tests/benchmark_report.c)
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/benchmark_report_test.c \
		tests/benchmark_report.c $(LDFLAGS) $(AUDIT_LDFLAGS) -o $@

$(PROBE_TARGET): $(PROBE_SOURCE) | $(BUILD_DIR)
	$(call generate_dependencies,$(PROBE_SOURCE))
	$(CC) $(CPPFLAGS) $(CFLAGS) $(PROBE_SOURCE) $(LDFLAGS) \
		$(AUDIT_LDFLAGS) -o $@

$(FAULT_TARGET): $(FAULT_SOURCES) | $(BUILD_DIR)
	$(call generate_dependencies,$(FAULT_SOURCES))
	$(CC) $(CPPFLAGS) $(CFLAGS) $(FAULT_SOURCES) $(LDFLAGS) \
		$(AUDIT_LDFLAGS) -o $@

$(SANITIZE_TARGET): $(SOURCES) | $(BUILD_DIR)
	$(call generate_dependencies,$(SOURCES))
	clang $(CPPFLAGS) -O1 -g -std=c17 -Wall -Wextra -Wpedantic -Werror \
		-fsanitize=address,undefined $(SOURCES) $(LDFLAGS) -o $@

$(FUZZ_SMOKE_TARGET): tests/lexer_fuzz.c src/posix_lexer.c src/posix_parser.c src/native_plan.c src/shell_aliases.c src/alias_expansion.c src/shell_functions.c src/shell_variables.c src/builtin_common.c src/async_repl.c src/resource_actions.c | $(BUILD_DIR)
	$(call generate_dependencies,tests/lexer_fuzz.c src/posix_lexer.c src/posix_parser.c src/native_plan.c src/shell_aliases.c src/alias_expansion.c src/shell_functions.c src/shell_variables.c src/builtin_common.c src/async_repl.c src/resource_actions.c,-DGSH_FUZZ_STANDALONE)
	$(CC) $(CPPFLAGS) -DGSH_FUZZ_STANDALONE $(CFLAGS) \
		tests/lexer_fuzz.c src/posix_lexer.c src/posix_parser.c \
		src/native_plan.c src/shell_aliases.c src/alias_expansion.c \
		src/shell_functions.c src/shell_variables.c \
		src/builtin_common.c src/async_repl.c src/resource_actions.c \
		$(LDFLAGS) $(AUDIT_LDFLAGS) -o $@

$(FUZZ_TARGET): tests/lexer_fuzz.c src/posix_lexer.c src/posix_parser.c src/native_plan.c src/shell_aliases.c src/alias_expansion.c src/shell_functions.c src/shell_variables.c src/builtin_common.c src/async_repl.c src/resource_actions.c | $(BUILD_DIR)
	$(call generate_dependencies,tests/lexer_fuzz.c src/posix_lexer.c src/posix_parser.c src/native_plan.c src/shell_aliases.c src/alias_expansion.c src/shell_functions.c src/shell_variables.c src/builtin_common.c src/async_repl.c src/resource_actions.c)
	$(FUZZ_CC) $(CPPFLAGS) -O1 -g -std=c17 -Wall -Wextra -Wpedantic \
		-Werror -fsanitize=fuzzer,address,undefined \
		tests/lexer_fuzz.c src/posix_lexer.c src/posix_parser.c \
		src/native_plan.c src/shell_aliases.c src/alias_expansion.c \
		src/shell_functions.c src/shell_variables.c \
		src/builtin_common.c src/async_repl.c src/resource_actions.c \
		$(LDFLAGS) -o $@

$(POLICY_TARGET): tests/source_policy.c tests/canon_call_graph.c \
		tests/canon_call_graph.h tests/canon_preprocessor.c \
		tests/canon_preprocessor.h tests/canon_symbols.c \
		tests/canon_symbols.h | $(BUILD_DIR)
	$(call generate_dependencies,tests/source_policy.c tests/canon_call_graph.c tests/canon_preprocessor.c tests/canon_symbols.c)
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/source_policy.c \
		tests/canon_call_graph.c tests/canon_preprocessor.c \
		tests/canon_symbols.c \
		$(LDFLAGS) $(AUDIT_LDFLAGS) -o $@

$(CONFORMANCE_TARGET): tests/posix_conformance.c | $(BUILD_DIR)
	$(call generate_dependencies,tests/posix_conformance.c)
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/posix_conformance.c $(LDFLAGS) \
		$(AUDIT_LDFLAGS) -o $@

$(VARIABLE_TEST_TARGET): tests/shell_variables_test.c src/shell_variables.c | $(BUILD_DIR)
	$(call generate_dependencies,tests/shell_variables_test.c src/shell_variables.c)
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/shell_variables_test.c \
	src/shell_variables.c $(LDFLAGS) $(AUDIT_LDFLAGS) -o $@

$(COMMAND_CACHE_TEST_TARGET): tests/command_cache_test.c \
		src/command_cache.c | $(BUILD_DIR)
	$(call generate_dependencies,tests/command_cache_test.c src/command_cache.c)
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/command_cache_test.c \
		src/command_cache.c $(LDFLAGS) $(AUDIT_LDFLAGS) -o $@

$(COMMAND_CACHE_SANITIZE_TEST_TARGET): tests/command_cache_test.c \
		src/command_cache.c | $(BUILD_DIR)
	$(call generate_dependencies,tests/command_cache_test.c src/command_cache.c)
	clang $(CPPFLAGS) -O1 -g -std=c17 -Wall -Wextra -Wpedantic \
		-Werror -fsanitize=address,undefined \
		tests/command_cache_test.c src/command_cache.c $(LDFLAGS) -o $@

$(POSITIONAL_TEST_TARGET): tests/positional_parameters_test.c src/positional_parameters.c | $(BUILD_DIR)
	$(call generate_dependencies,tests/positional_parameters_test.c src/positional_parameters.c)
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/positional_parameters_test.c \
		src/positional_parameters.c $(LDFLAGS) $(AUDIT_LDFLAGS) -o $@

$(BACKGROUND_TEST_TARGET): tests/background_jobs_test.c src/background_jobs.c | $(BUILD_DIR)
	$(call generate_dependencies,tests/background_jobs_test.c src/background_jobs.c)
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/background_jobs_test.c \
	src/background_jobs.c $(LDFLAGS) $(AUDIT_LDFLAGS) -o $@

$(ALIAS_TEST_TARGET): tests/shell_aliases_test.c src/shell_aliases.c \
		src/alias_expansion.c src/posix_lexer.c src/posix_parser.c | $(BUILD_DIR)
	$(call generate_dependencies,tests/shell_aliases_test.c src/shell_aliases.c src/alias_expansion.c src/posix_lexer.c src/posix_parser.c)
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/shell_aliases_test.c \
		src/shell_aliases.c src/alias_expansion.c src/posix_lexer.c \
		src/posix_parser.c $(LDFLAGS) $(AUDIT_LDFLAGS) -o $@

$(FUNCTION_TEST_TARGET): tests/shell_functions_test.c src/shell_functions.c \
		src/posix_lexer.c src/posix_parser.c | $(BUILD_DIR)
	$(call generate_dependencies,tests/shell_functions_test.c src/shell_functions.c src/posix_lexer.c src/posix_parser.c)
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/shell_functions_test.c \
		src/shell_functions.c src/posix_lexer.c src/posix_parser.c \
		$(LDFLAGS) $(AUDIT_LDFLAGS) -o $@

$(FUNCTION_SANITIZE_TEST_TARGET): tests/shell_functions_test.c \
		src/shell_functions.c src/posix_lexer.c src/posix_parser.c | $(BUILD_DIR)
	$(call generate_dependencies,tests/shell_functions_test.c src/shell_functions.c src/posix_lexer.c src/posix_parser.c)
	clang $(CPPFLAGS) -O1 -g -std=c17 -Wall -Wextra -Wpedantic \
		-Werror -fsanitize=address,undefined \
		tests/shell_functions_test.c src/shell_functions.c \
		src/posix_lexer.c src/posix_parser.c $(LDFLAGS) -o $@

$(CONFIG_TEST_TARGET): tests/shell_config_test.c src/shell_config.c | $(BUILD_DIR)
	$(call generate_dependencies,tests/shell_config_test.c src/shell_config.c)
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/shell_config_test.c \
		src/shell_config.c $(LDFLAGS) $(AUDIT_LDFLAGS) -o $@

$(SOURCE_WORKSPACE_TEST_TARGET): tests/source_workspace_test.c \
		src/source_workspace.c src/shell_aliases.c \
		src/shell_functions.c | $(BUILD_DIR)
	$(call generate_dependencies,tests/source_workspace_test.c src/source_workspace.c src/shell_aliases.c src/shell_functions.c)
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/source_workspace_test.c \
		src/source_workspace.c src/shell_aliases.c \
		src/shell_functions.c $(LDFLAGS) $(AUDIT_LDFLAGS) -o $@

$(SHELL_TRAPS_TEST_TARGET): tests/shell_traps_test.c \
		src/shell_traps.c | $(BUILD_DIR)
	$(call generate_dependencies,tests/shell_traps_test.c src/shell_traps.c)
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/shell_traps_test.c \
		src/shell_traps.c $(LDFLAGS) $(AUDIT_LDFLAGS) -o $@

$(SHELL_TRAPS_SANITIZE_TEST_TARGET): tests/shell_traps_test.c \
		src/shell_traps.c | $(BUILD_DIR)
	$(call generate_dependencies,tests/shell_traps_test.c src/shell_traps.c)
	clang $(CPPFLAGS) -O1 -g -std=c17 -Wall -Wextra -Wpedantic \
		-Werror -fsanitize=address,undefined \
		tests/shell_traps_test.c src/shell_traps.c $(LDFLAGS) -o $@

check: $(TARGET) $(HISTORY_AGENT_TARGET) $(TEST_TARGET) $(PROBE_TARGET) \
		$(FILE_BUILTINS_TEST_TARGET) $(RESOURCE_ACTIONS_TEST_TARGET)
	$(abspath $(TEST_TARGET)) $(abspath $(TARGET))
	$(abspath $(FILE_BUILTINS_TEST_TARGET))
	$(abspath $(RESOURCE_ACTIONS_TEST_TARGET))

check-fault: $(FAULT_TARGET) $(TEST_TARGET)
	$(abspath $(TEST_TARGET)) --fault $(abspath $(FAULT_TARGET))

check-resource: $(TARGET) $(TEST_TARGET)
	$(abspath $(TEST_TARGET)) --resource $(abspath $(TARGET))

check-sanitize: $(SANITIZE_TARGET) $(HISTORY_AGENT_TARGET) \
		$(FUNCTION_SANITIZE_TEST_TARGET) \
		$(COMMAND_CACHE_SANITIZE_TEST_TARGET) \
		$(SHELL_TRAPS_SANITIZE_TEST_TARGET) \
		$(FILE_BUILTINS_SANITIZE_TEST_TARGET) \
		$(RESOURCE_ACTIONS_SANITIZE_TEST_TARGET) \
		$(TEST_TARGET) $(PROBE_TARGET)
	ASAN_OPTIONS=abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
		$(abspath $(TEST_TARGET)) $(abspath $(SANITIZE_TARGET))
	ASAN_OPTIONS=abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
		$(abspath $(FUNCTION_SANITIZE_TEST_TARGET))
	ASAN_OPTIONS=abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
		$(abspath $(COMMAND_CACHE_SANITIZE_TEST_TARGET))
	ASAN_OPTIONS=abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
		$(abspath $(SHELL_TRAPS_SANITIZE_TEST_TARGET))
	ASAN_OPTIONS=abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
		$(abspath $(FILE_BUILTINS_SANITIZE_TEST_TARGET))
	ASAN_OPTIONS=abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
		$(abspath $(RESOURCE_ACTIONS_SANITIZE_TEST_TARGET))

fuzz-smoke: $(FUZZ_SMOKE_TARGET)
	$(abspath $(FUZZ_SMOKE_TARGET)) $(FUZZ_CASES)

fuzz-sanitize: | $(BUILD_DIR)
	clang $(CPPFLAGS) -DGSH_FUZZ_STANDALONE -O1 -g -std=c17 \
		-Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined \
		tests/lexer_fuzz.c src/posix_lexer.c src/posix_parser.c \
		src/native_plan.c src/shell_aliases.c src/alias_expansion.c \
		src/shell_functions.c src/shell_variables.c \
		src/builtin_common.c src/async_repl.c src/resource_actions.c \
		-o $(FUZZ_SMOKE_TARGET)-sanitize
	ASAN_OPTIONS=abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
		$(abspath $(FUZZ_SMOKE_TARGET))-sanitize $(FUZZ_CASES)

fuzz-libfuzzer: $(FUZZ_TARGET) | $(FUZZ_WORK_CORPUS)
	ASAN_OPTIONS=abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
		$(abspath $(FUZZ_TARGET)) $(FUZZ_WORK_CORPUS) $(FUZZ_SEED_CORPUS) \
		-max_total_time=$(FUZZ_SECONDS) \
		-print_final_stats=1

fuzz-libfuzzer-linux:
	docker build --platform linux/amd64 \
		-f dev/quality-linux.Dockerfile -t $(LINUX_QUALITY_IMAGE) .
	docker run --rm --platform linux/amd64 \
		--mount "type=bind,source=$(CURDIR),target=/workspace,readonly" \
		-w /workspace $(LINUX_QUALITY_IMAGE) sh -ec '\
		make -j1 FUZZ_CC=clang BUILD_DIR=/tmp/gsh-fuzz \
			FUZZ_WORK_CORPUS=/tmp/gsh-fuzz-corpus \
			FUZZ_SECONDS=$(FUZZ_SECONDS) fuzz-libfuzzer'

$(FUZZ_WORK_CORPUS):
	mkdir -p $@

fuzz-pty: $(TARGET) $(TEST_TARGET) $(PROBE_TARGET)
	$(abspath $(TEST_TARGET)) --fuzz-pty $(abspath $(TARGET)) $(PTY_FUZZ_CASES)

fuzz-pty-sanitize: $(SANITIZE_TARGET) $(TEST_TARGET) $(PROBE_TARGET)
	ASAN_OPTIONS=abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
		$(abspath $(TEST_TARGET)) --fuzz-pty $(abspath $(SANITIZE_TARGET)) \
		$(PTY_FUZZ_CASES)

policy: $(TARGET) $(POLICY_TARGET) $(TARGET_MANIFEST)
	$(abspath $(POLICY_TARGET)) $(CURDIR) $(abspath $(TARGET)) \
		$(abspath $(TARGET_MANIFEST))

quality: policy analyze quality-dependencies quality-includes quality-maps

quality-callgraph: $(TARGET) $(POLICY_TARGET) $(TARGET_MANIFEST)
	$(abspath $(POLICY_TARGET)) $(CURDIR) $(abspath $(TARGET)) \
		$(abspath $(TARGET_MANIFEST)) --verbose

quality-compilers:
	@matrix_dir=$$(mktemp -d /tmp/gsh-quality.XXXXXX); \
	trap 'rm -rf "$$matrix_dir"' EXIT HUP INT TERM; \
	$(MAKE) -j1 CC="$(QUALITY_CLANG_CC)" \
		BUILD_DIR="$$matrix_dir/clang" policy quality-dependencies \
		quality-includes quality-maps; \
	$(MAKE) -j1 CC="$(QUALITY_GCC_CC)" \
		BUILD_DIR="$$matrix_dir/gcc" policy quality-dependencies \
		quality-includes quality-maps

quality-linux:
	docker build --platform linux/amd64 \
		-f dev/quality-linux.Dockerfile -t $(LINUX_QUALITY_IMAGE) .
	docker run --rm --platform linux/amd64 \
		--mount "type=bind,source=$(CURDIR),target=/workspace,readonly" \
		-w /workspace $(LINUX_QUALITY_IMAGE) sh -ec '\
		test "$$(uname -m)" = x86_64; \
		make -j1 CC=gcc BUILD_DIR=/tmp/gsh-gcc \
			policy quality-dependencies quality-includes quality-maps \
			conformance; \
		make -j1 CC=clang BUILD_DIR=/tmp/gsh-clang \
			policy quality-dependencies quality-includes quality-maps \
			conformance'

quality-dependencies: $(QUALITY_TARGETS)
	@failed=0; checked=0; \
	for target in $(QUALITY_TARGETS); do \
		dependency="$$target.d"; checked=$$((checked + 1)); \
		if test ! -s "$$dependency" || \
		   ! grep -Fq "$$target:" "$$dependency"; then \
			printf 'missing dependency manifest: %s\n' "$$dependency"; \
			failed=1; \
		elif ! $(MAKE) -q "$$target"; then \
			printf 'stale generated dependency graph: %s\n' "$$target"; \
			failed=1; \
		fi; \
	done; \
	if test "$$failed" -ne 0; then exit 1; fi; \
	printf 'generated dependencies: %s target manifests are current\n' \
		"$$checked"

quality-includes:
	@include_dir=$$(mktemp -d /tmp/gsh-includes.XXXXXX); \
	trap 'rm -rf "$$include_dir"' EXIT HUP INT TERM; \
	compiler_family=$$($(CC) --version 2>/dev/null | sed -n '1p'); \
	case "$$compiler_family" in \
		*clang*) compiler_family=clang ;; \
		*GCC*|*gcc*) compiler_family=gcc ;; \
		*) compiler_family=unknown ;; \
	esac; \
	failed=0; checked=0; deferred=0; inactive=0; \
	for source in $$(printf '%s\n' $(CANON_C_SOURCES) | sort -u); do \
		sequence=0; source_flags=; \
		if test "$$source" = tests/lexer_fuzz.c; then \
			source_flags=-DGSH_FUZZ_STANDALONE; \
		fi; \
		for line in $$(awk '/^[ \t]*#[ \t]*include[ \t]*[<"]/{print NR}' \
			"$$source"); do \
			sequence=$$((sequence + 1)); \
			declaration=$$(sed -n "$${line}p" "$$source"); \
			ownership=$$(printf '%s\n' "$$declaration" | sed -n \
				's/.*CANON-INCLUDE: \([a-z][a-z]*\).*/\1/p'); \
			if test -n "$$ownership" && test "$$ownership" != linux && \
			   test "$$ownership" != macos && \
			   test "$$ownership" != gcc && \
			   test "$$ownership" != clang; then \
				printf 'unknown include ownership: %s:%s (%s)\n' \
					"$$source" "$$line" "$$ownership"; \
				failed=1; continue; \
			fi; \
			target_system=; \
			if test "$$ownership" = linux; then target_system=Linux; fi; \
			if test "$$ownership" = macos; then target_system=Darwin; fi; \
			if test -n "$$target_system" && \
			   test '$(UNAME_SYSTEM)' != "$$target_system"; then \
				deferred=$$((deferred + 1)); continue; \
			fi; \
			if { test "$$ownership" = gcc || test "$$ownership" = clang; } && \
			   test "$$compiler_family" != "$$ownership"; then \
				deferred=$$((deferred + 1)); continue; \
			fi; \
			active="$$include_dir/active-$$sequence.c"; \
			awk -v replace="$$line" \
				'NR == replace { print "#error CANON_ACTIVE_INCLUDE"; next } \
				 { print }' "$$source" > "$$active"; \
			if $(CC) $(CPPFLAGS) $(CFLAGS) $(SODIUM_CFLAGS) \
				$$source_flags \
				-Isrc -Itests -iquote "$$(dirname "$$source")" \
				-fsyntax-only "$$active" >/dev/null 2>&1; then \
				inactive=$$((inactive + 1)); continue; \
			fi; \
			checked=$$((checked + 1)); \
			candidate="$$include_dir/candidate-$$sequence.c"; \
			awk -v skip="$$line" 'NR != skip { print }' "$$source" \
				> "$$candidate"; \
			if $(CC) $(CPPFLAGS) $(CFLAGS) $(SODIUM_CFLAGS) \
				$$source_flags \
				-Isrc -Itests -iquote "$$(dirname "$$source")" \
				-fsyntax-only "$$candidate" >/dev/null 2>&1; then \
				printf 'unused include: %s:%s\n' "$$source" "$$line"; \
				failed=1; \
			fi; \
		done; \
	done; \
	if test "$$failed" -ne 0; then exit 1; fi; \
	printf 'include isolation: %s active direct includes are necessary; %s platform/toolchain-owned and %s inactive checks deferred\n' \
		"$$checked" "$$deferred" "$$inactive"

$(QUALITY_MAPS): %.map: %
	@if test ! -s "$@"; then $(MAKE) -B "$<"; fi
	@test -s "$@"

quality-maps: $(QUALITY_MAPS) $(TARGET_MANIFEST)
	$(abspath $(POLICY_TARGET)) $(CURDIR) $(abspath $(TARGET)) \
		$(abspath $(TARGET_MANIFEST)) \
		--maps $(abspath $(QUALITY_MAPS))
	@printf 'quality maps: verified %s linker maps in %s\n' \
		'$(words $(QUALITY_MAPS))' '$(abspath $(BUILD_DIR))'

analyze:
	@set -e; for source in $(CANON_C_SOURCES); do \
		$(ANALYZE_CC) $(CPPFLAGS) $(CFLAGS) $(SODIUM_CFLAGS) \
			--analyze -Xanalyzer -analyzer-werror "$$source" \
			-o /dev/null; \
	done
	@printf 'static analysis: %s manifest-owned C sources passed\n' \
		'$(words $(CANON_C_SOURCES))'

conformance: $(TARGET) $(CONFORMANCE_TARGET)
	$(abspath $(CONFORMANCE_TARGET)) $(abspath $(TARGET)) $(BASH_BIN)

check-variables: $(VARIABLE_TEST_TARGET)
	$(abspath $(VARIABLE_TEST_TARGET))

check-command-cache: $(COMMAND_CACHE_TEST_TARGET)
	$(abspath $(COMMAND_CACHE_TEST_TARGET))

check-positionals: $(POSITIONAL_TEST_TARGET)
	$(abspath $(POSITIONAL_TEST_TARGET))

check-background: $(BACKGROUND_TEST_TARGET)
	$(abspath $(BACKGROUND_TEST_TARGET))

check-aliases: $(ALIAS_TEST_TARGET)
	$(abspath $(ALIAS_TEST_TARGET))

check-functions: $(FUNCTION_TEST_TARGET)
	$(abspath $(FUNCTION_TEST_TARGET))

check-config: $(CONFIG_TEST_TARGET)
	$(abspath $(CONFIG_TEST_TARGET))

check-file-builtins: $(FILE_BUILTINS_TEST_TARGET)
	$(abspath $(FILE_BUILTINS_TEST_TARGET))

check-resource-actions: $(RESOURCE_ACTIONS_TEST_TARGET)
	$(abspath $(RESOURCE_ACTIONS_TEST_TARGET))

check-source-workspaces: $(SOURCE_WORKSPACE_TEST_TARGET)
	$(abspath $(SOURCE_WORKSPACE_TEST_TARGET))

check-traps: $(SHELL_TRAPS_TEST_TARGET)
	$(abspath $(SHELL_TRAPS_TEST_TARGET))

check-benchmark-report: $(BENCHMARK_REPORT_TEST_TARGET)
	$(abspath $(BENCHMARK_REPORT_TEST_TARGET))

soak: $(TARGET) $(TEST_TARGET)
	$(abspath $(TEST_TARGET)) --soak $(abspath $(TARGET)) $(SOAK_SECONDS)

verify-fast:
	$(MAKE) -j1 quality
	$(MAKE) -j1 conformance
	$(MAKE) -j1 check
	$(MAKE) -j1 check-variables
	$(MAKE) -j1 check-command-cache
	$(MAKE) -j1 check-positionals
	$(MAKE) -j1 check-background
	$(MAKE) -j1 check-config
	$(MAKE) -j1 check-source-workspaces
	$(MAKE) -j1 check-traps
	$(MAKE) -j1 check-benchmark-report
	$(MAKE) -j1 check-aliases
	$(MAKE) -j1 check-functions
	$(MAKE) -j1 check-fault
	$(MAKE) -j1 check-resource
	$(MAKE) -j1 fuzz-smoke
	$(MAKE) -j1 fuzz-sanitize
	$(MAKE) -j1 fuzz-pty
	$(MAKE) -j1 check-sanitize

bench: $(TARGET) $(TEST_TARGET) | $(PERFORMANCE_DIR)
	@GSH_BENCH_REVISION="$(BENCH_REVISION)" \
	GSH_BENCH_COMPILER="$$($(CC) --version | sed -n '1p')" \
	GSH_BENCH_BASH_VERSION="$$($(BASH_BIN) --version | sed -n '1p')" \
	GSH_BENCH_ZSH_VERSION="$$($(ZSH_BIN) --version | sed -n '1p')" \
	$(abspath $(TEST_TARGET)) --benchmark $(abspath $(TARGET)) $(BASH_BIN) $(ZSH_BIN) \
		$(abspath $(BENCH_OUTPUT))

bench-record: bench

bench-alias: $(TARGET) $(TEST_TARGET) | $(PERFORMANCE_DIR)
	@GSH_BENCH_REVISION="$(BENCH_REVISION)" \
	GSH_BENCH_COMPILER="$$($(CC) --version | sed -n '1p')" \
	GSH_BENCH_BASH_VERSION="$$($(BASH_BIN) --version | sed -n '1p')" \
	GSH_BENCH_ZSH_VERSION="$$($(ZSH_BIN) --version | sed -n '1p')" \
	$(abspath $(TEST_TARGET)) --benchmark-alias $(abspath $(TARGET)) $(BASH_BIN) \
		$(ZSH_BIN) $(abspath $(ALIAS_BENCH_OUTPUT))

bench-alias-record: bench-alias

$(PERFORMANCE_DIR):
	mkdir -p $@

$(BUILD_DIR):
	mkdir -p $@

clean:
	rm -f $(TARGET) $(HISTORY_AGENT_TARGET) $(TEST_TARGET) $(PROBE_TARGET) $(FAULT_TARGET)
	rm -f $(SANITIZE_TARGET) $(FUZZ_SMOKE_TARGET) $(FUZZ_TARGET) $(POLICY_TARGET)
	rm -f $(CONFORMANCE_TARGET)
	rm -f $(VARIABLE_TEST_TARGET)
	rm -f $(BENCHMARK_REPORT_TEST_TARGET)
	rm -f $(COMMAND_CACHE_TEST_TARGET)
	rm -f $(COMMAND_CACHE_SANITIZE_TEST_TARGET)
	rm -f $(POSITIONAL_TEST_TARGET)
	rm -f $(BACKGROUND_TEST_TARGET)
	rm -f $(CONFIG_TEST_TARGET)
	rm -f $(SOURCE_WORKSPACE_TEST_TARGET)
	rm -f $(SHELL_TRAPS_TEST_TARGET)
	rm -f $(SHELL_TRAPS_SANITIZE_TEST_TARGET)
	rm -f $(ALIAS_TEST_TARGET)
	rm -f $(FUNCTION_TEST_TARGET)
	rm -f $(FUNCTION_SANITIZE_TEST_TARGET)
	rm -f $(BUILD_DIR)/posix_parser.o $(BUILD_DIR)/pty-smoke
	rm -f $(FUZZ_SMOKE_TARGET)-sanitize
	rm -f $(QUALITY_MAPS)
	rm -f $(DEPENDENCY_FILES) $(addsuffix .tmp,$(DEPENDENCY_FILES))
	rm -rf $(TARGET).dSYM $(TEST_TARGET).dSYM $(PROBE_TARGET).dSYM
	rm -rf $(BUILD_DIR)/gsh-sanitize.dSYM
	rmdir $(BUILD_DIR) 2>/dev/null || true
