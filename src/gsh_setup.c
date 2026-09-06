#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif
#if _POSIX_C_SOURCE < 202405L
#error "gsh requires the POSIX.1-2024 feature-test baseline"
#endif

#include "llm_hardware.h"
#include "llm_json.h"
#include "shell_config.h"

#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h> /* CANON-INCLUDE: linux */
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h> /* CANON-INCLUDE: linux */
#include <termios.h>
#include <unistd.h>

enum {
    SETUP_FILE_CAP = 65536,
    SETUP_INPUT_CAP = 1024,
    SETUP_MENU_CAP = 16,
    SETUP_HTTP_CAP = 65536,
};

typedef enum {
    SETUP_DS4 = 0,
    SETUP_LLAMA_CPP,
    SETUP_CUSTOM,
    SETUP_DISABLED,
} setup_recipe;

typedef struct {
    setup_recipe recipe;
    bool managed;
    char name[GSH_LLM_PROVIDER_NAME_CAP];
    char base_url[GSH_LLM_BASE_URL_CAP];
    char model[GSH_LLM_MODEL_CAP];
    char credential[GSH_LLM_CREDENTIAL_CAP];
    char runtime_command[GSH_LLM_RUNTIME_COMMAND_CAP];
} setup_selection;

static int write_all(int descriptor, const char *text, size_t length)
{
    size_t offset = 0U;

    if (descriptor < 0 || text == NULL) return -1;
    while (offset < length) {
        ssize_t count = write(descriptor, text + offset, length - offset);
        if (count > 0) offset += (size_t)count;
        else if (count == -1 && errno == EINTR) continue;
        else return -1;
    }
    return 0;
}

static int read_line(const char *label, char *output, size_t capacity,
                     const char *fallback)
{
    size_t length;

    if (label == NULL || output == NULL || capacity < 2U) return -1;
    (void)fputs(label, stdout);
    if (fallback != NULL) (void)printf(" [%s]", fallback);
    (void)fputs(": ", stdout);
    (void)fflush(stdout);
    if (fgets(output, (int)capacity, stdin) == NULL) return -1;
    length = strlen(output);
    while (length > 0U && (output[length - 1U] == '\n' ||
                           output[length - 1U] == '\r')) output[--length] = '\0';
    if (length == 0U && fallback != NULL) {
        if (strlen(fallback) >= capacity) return -1;
        (void)memcpy(output, fallback, strlen(fallback) + 1U);
    }
    return output[0] == '\0' ? -1 : 0;
}

static bool yes_no(const char *question, bool default_yes)
{
    char answer[16];

    if (question == NULL) return false;
    (void)printf("%s [%s]: ", question, default_yes ? "Y/n" : "y/N");
    (void)fflush(stdout);
    if (fgets(answer, sizeof(answer), stdin) == NULL) return false;
    if (answer[0] == '\n' || answer[0] == '\r') return default_yes;
    return answer[0] == 'y' || answer[0] == 'Y';
}

static void draw_menu(const char *const options[], size_t count,
                      size_t selected, bool redraw)
{
    size_t index;

    if (options == NULL || count == 0U || count > SETUP_MENU_CAP) return;
    if (redraw) (void)printf("\033[%zuA", count);
    for (index = 0U; index < count; index++)
        (void)printf("\r\033[2K %s %s\n", index == selected ? ">" : " ",
                     options[index]);
    (void)fflush(stdout);
}

static int numeric_menu(const char *const options[], size_t count)
{
    char answer[32];
    char *end = NULL;
    unsigned long choice;
    size_t index;

    if (options == NULL || count == 0U || count > SETUP_MENU_CAP) return -1;
    for (index = 0U; index < count; index++)
        (void)printf("  %zu) %s\n", index + 1U, options[index]);
    (void)fputs("Choice: ", stdout);
    if (fgets(answer, sizeof(answer), stdin) == NULL) return -1;
    errno = 0;
    choice = strtoul(answer, &end, 10);
    if (errno != 0 || end == answer || choice == 0U || choice > count)
        return -1;
    while (*end == ' ' || *end == '\t') end++;
    if (*end != '\0' && *end != '\n' && *end != '\r') return -1;
    return (int)(choice - 1U);
}

static int arrow_menu(const char *const options[], size_t count)
{
    struct termios original;
    struct termios raw;
    size_t selected = 0U;
    unsigned int escape = 0U;
    unsigned int steps;
    bool accepted = false;

    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO) ||
        tcgetattr(STDIN_FILENO, &original) == -1) return numeric_menu(options, count);
    raw = original;
    raw.c_lflag &= (tcflag_t)~(ICANON | ECHO | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == -1)
        return numeric_menu(options, count);
    draw_menu(options, count, selected, false);
    for (steps = 0U; steps < 4096U; steps++) {
        unsigned char byte;
        if (read(STDIN_FILENO, &byte, 1U) != 1) break;
        if (escape == 0U && (byte == '\r' || byte == '\n')) {
            accepted = true;
            break;
        }
        if (escape == 0U && (byte == 0x03U || byte == 0x04U)) break;
        if (escape == 0U && byte == 0x1bU) escape = 1U;
        else if (escape == 1U && byte == '[') escape = 2U;
        else if (escape == 2U && (byte == 'A' || byte == 'B')) {
            if (byte == 'A') selected = selected == 0U ? count - 1U : selected - 1U;
            else selected = selected + 1U == count ? 0U : selected + 1U;
            draw_menu(options, count, selected, true);
            escape = 0U;
        } else escape = 0U;
    }
    (void)tcsetattr(STDIN_FILENO, TCSANOW, &original);
    return accepted ? (int)selected : -1;
}

static bool environment_name_valid(const char *name)
{
    size_t index;

    if (name == NULL || !((name[0] >= 'A' && name[0] <= 'Z') ||
                          (name[0] >= 'a' && name[0] <= 'z') ||
                          name[0] == '_')) return false;
    for (index = 1U; index < GSH_LLM_CREDENTIAL_CAP; index++) {
        unsigned char byte = (unsigned char)name[index];
        if (byte == '\0') return true;
        if (!((byte >= 'A' && byte <= 'Z') ||
              (byte >= 'a' && byte <= 'z') ||
              (byte >= '0' && byte <= '9') || byte == '_')) return false;
    }
    return false;
}

static bool config_value_valid(const char *value)
{
    size_t index;

    if (value == NULL || value[0] == '\0') return false;
    for (index = 0U; index < SETUP_INPUT_CAP && value[index] != '\0'; index++)
        if ((unsigned char)value[index] < 0x20U) return false;
    return index < SETUP_INPUT_CAP;
}

static bool endpoint_is_safe(const char *url)
{
    const char *host;

    if (url == NULL) return false;
    if (strncmp(url, "https://", 8U) == 0) return true;
    if (strncmp(url, "http://", 7U) != 0) return false;
    host = url + 7U;
    return (strncmp(host, "127.0.0.1", 9U) == 0 &&
            (host[9] == '\0' || host[9] == ':' || host[9] == '/')) ||
           (strncmp(host, "localhost", 9U) == 0 &&
            (host[9] == '\0' || host[9] == ':' || host[9] == '/')) ||
           (strncmp(host, "[::1]", 5U) == 0 &&
            (host[5] == '\0' || host[5] == ':' || host[5] == '/'));
}

/* ── Setup Rewrites One Declarative Authority ───────────────────
 * Provider installers used to imply hidden defaults outside the shell config.
 * The wizard now detects an existing endpoint first, and every chosen model,
 * credential reference, and managed launch command is written to ~/.gshrc.
 * It preserves unrelated settings through an atomic replacement and never
 * stores a bearer token, so runtime behavior can be audited from one file.
 * ─────────────────────────────────────────────────────────────── */
static int configure_custom(setup_selection *selection)
{
    char environment[SETUP_INPUT_CAP];

    if (selection == NULL ||
        read_line("Provider name", selection->name, sizeof(selection->name),
                  "custom") == -1 ||
        read_line("Responses API base URL", selection->base_url,
                  sizeof(selection->base_url), "https://api.openai.com/v1") == -1 ||
        read_line("Model", selection->model, sizeof(selection->model), NULL) == -1)
        return -1;
    if (yes_no("Does this endpoint require a bearer API key?", true)) {
        if (read_line("Environment variable containing the key", environment,
                      sizeof(environment), "OPENAI_API_KEY") == -1 ||
            !environment_name_valid(environment) ||
            snprintf(selection->credential, sizeof(selection->credential),
                     "env:%s", environment) >=
                (int)sizeof(selection->credential)) return -1;
    } else (void)memcpy(selection->credential, "none", sizeof("none"));
    return config_value_valid(selection->name) &&
                   config_value_valid(selection->base_url) &&
                   config_value_valid(selection->model) &&
                   endpoint_is_safe(selection->base_url)
               ? 0 : -1;
}

static void configure_recipe(setup_selection *selection, setup_recipe recipe,
                             const char *home)
{
    if (selection == NULL || home == NULL) return;
    (void)memset(selection, 0, sizeof(*selection));
    selection->recipe = recipe;
    (void)memcpy(selection->credential, "none", sizeof("none"));
    if (recipe == SETUP_DS4) {
        (void)memcpy(selection->name, "ds4", sizeof("ds4"));
        (void)memcpy(selection->base_url, "http://127.0.0.1:8000/v1",
                     sizeof("http://127.0.0.1:8000/v1"));
        (void)memcpy(selection->model, "deepseek-v4-flash",
                     sizeof("deepseek-v4-flash"));
        (void)snprintf(selection->runtime_command,
                       sizeof(selection->runtime_command),
                       "%s/.genshell/runtimes/ds4/ds4-server --chdir "
                       "%s/.genshell/runtimes/ds4 --port 8000 --ctx 100000",
                       home, home);
    } else if (recipe == SETUP_LLAMA_CPP) {
        (void)memcpy(selection->name, "llama-cpp", sizeof("llama-cpp"));
        (void)memcpy(selection->base_url, "http://127.0.0.1:8080/v1",
                     sizeof("http://127.0.0.1:8080/v1"));
        (void)memcpy(selection->model, "ggml-org/gemma-3-1b-it-GGUF",
                     sizeof("ggml-org/gemma-3-1b-it-GGUF"));
        (void)snprintf(selection->runtime_command,
                       sizeof(selection->runtime_command),
                       "%s/.genshell/runtimes/llama.cpp/build/bin/llama-server "
                       "--host 127.0.0.1 --port 8080",
                       home);
    }
}

static unsigned int bytes_to_gib(uint64_t bytes)
{
    return (unsigned int)(bytes / 1073741824U);
}

static void print_hardware(const gsh_llm_hardware *hardware)
{
    const char *backend;

    if (hardware == NULL) return;
    backend = gsh_llm_accelerator_name(hardware->accelerator);
    (void)printf("Detected: %s %s, %u GiB RAM, %u logical CPUs\n",
                 hardware->operating_system, hardware->architecture,
                 bytes_to_gib(hardware->ram_bytes), hardware->logical_cpus);
    if (hardware->accelerator == GSH_LLM_ACCELERATOR_METAL)
        (void)printf("Acceleration: %s (%s, unified memory)\n", backend,
                     hardware->device);
    else if (hardware->accelerator == GSH_LLM_ACCELERATOR_CPU)
        (void)puts("Acceleration: CPU only");
    else
        (void)printf("Acceleration: %s (%s, %u GiB VRAM)%s\n", backend,
                     hardware->device,
                     bytes_to_gib(hardware->accelerator_bytes),
                     hardware->accelerator_buildable
                         ? "" : "; toolkit unavailable, CPU build fallback");
    (void)printf("Free disk: %u GiB; recommended model budget: %u GiB\n\n",
                 bytes_to_gib(hardware->disk_free_bytes),
                 gsh_llm_model_budget_gib(hardware));
}

static int models_url(const setup_selection *selection, char *url,
                      size_t capacity)
{
    size_t length;
    int written;

    if (selection == NULL || url == NULL || capacity == 0U) return -1;
    length = strlen(selection->base_url);
    written = snprintf(url, capacity, "%s%smodels", selection->base_url,
                       length > 0U && selection->base_url[length - 1U] == '/'
                           ? "" : "/");
    return written >= 0 && (size_t)written < capacity ? 0 : -1;
}

static int health_child(int output, const setup_selection *selection)
{
    char url[GSH_LLM_BASE_URL_CAP + 32U];
    char authorization[GSH_LLM_CREDENTIAL_CAP + 4096U];
    struct curl_slist *headers = NULL;
    const char *key = NULL;
    CURL *curl;
    CURLcode status;

    if (selection == NULL || dup2(output, STDOUT_FILENO) == -1 ||
        close(output) == -1 || setvbuf(stdout, NULL, _IONBF, 0) != 0 ||
        models_url(selection, url, sizeof(url)) == -1)
        return 125;
    if (strncmp(selection->credential, "env:", 4U) == 0)
        key = getenv(selection->credential + 4U);
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) return 125;
    curl = curl_easy_init();
    if (curl == NULL) { curl_global_cleanup(); return 125; }
    if (key != NULL && key[0] != '\0') {
        if (snprintf(authorization, sizeof(authorization),
                     "Authorization: Bearer %s", key) >=
            (int)sizeof(authorization)) return 125;
        headers = curl_slist_append(headers, authorization);
        (void)curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }
    (void)curl_easy_setopt(curl, CURLOPT_URL, url);
    (void)curl_easy_setopt(curl, CURLOPT_TIMEOUT, 3L);
    (void)curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 2L);
    (void)curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    (void)curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    (void)curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    (void)curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    status = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    curl_global_cleanup();
    return status == CURLE_OK ? 0 : 1;
}

static bool endpoint_available(setup_selection *selection)
{
    char response[SETUP_HTTP_CAP];
    int channel[2] = {-1, -1};
    pid_t pid;
    size_t used = 0U;
    int status = 0;
    unsigned int reads;

    if (selection == NULL || pipe(channel) == -1) return false;
    pid = fork();
    if (pid == 0) {
        (void)close(channel[0]);
        _exit(health_child(channel[1], selection));
    }
    (void)close(channel[1]);
    if (pid < 0) { (void)close(channel[0]); return false; }
    for (reads = 0U; reads < 1024U && used + 1U < sizeof(response); reads++) {
        ssize_t count = read(channel[0], response + used,
                             sizeof(response) - used - 1U);
        if (count > 0) used += (size_t)count;
        else if (count == -1 && errno == EINTR) reads--;
        else break;
    }
    response[used] = '\0';
    (void)close(channel[0]);
    while (waitpid(pid, &status, 0) == -1 && errno == EINTR) {
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 ||
        strstr(response, "data") == NULL) return false;
    if (selection->recipe == SETUP_LLAMA_CPP)
        (void)gsh_json_get_string(response, used, "id", selection->model,
                                  sizeof(selection->model));
    return true;
}

static int run_program(const char *directory, char *const arguments[])
{
    pid_t pid;
    int status = 0;

    if (directory == NULL || arguments == NULL || arguments[0] == NULL)
        return -1;
    (void)printf("\n+ %s", arguments[0]);
    for (size_t index = 1U; arguments[index] != NULL && index < 16U; index++)
        (void)printf(" %s", arguments[index]);
    (void)fputc('\n', stdout);
    pid = fork();
    if (pid == 0) {
        if (chdir(directory) == -1) _exit(125);
        execvp(arguments[0], arguments);
        _exit(127);
    }
    if (pid < 0) return -1;
    while (waitpid(pid, &status, 0) == -1 && errno == EINTR) {
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

static int ensure_directory(const char *path)
{
    struct stat status;

    if (path == NULL) return -1;
    if (mkdir(path, 0700) == 0) return 0;
    if (errno != EEXIST || stat(path, &status) == -1 ||
        !S_ISDIR(status.st_mode) || status.st_uid != geteuid()) return -1;
    return 0;
}

static int runtime_root(const char *home, char *root, size_t capacity)
{
    char genshell[4096];

    if (home == NULL || root == NULL ||
        snprintf(genshell, sizeof(genshell), "%s/.genshell", home) >=
            (int)sizeof(genshell) ||
        snprintf(root, capacity, "%s/runtimes", genshell) >= (int)capacity ||
        ensure_directory(genshell) == -1 || ensure_directory(root) == -1)
        return -1;
    return 0;
}

static int clone_pinned(const char *root, const char *url, const char *name,
                        const char *revision)
{
    char destination[4096];
    char *clone[] = {(char *)"git", (char *)"clone",
                     (char *)"--filter=blob:none", (char *)"--no-checkout",
                     (char *)url, destination, NULL};
    char *checkout[] = {(char *)"git", (char *)"-C", destination,
                        (char *)"checkout", (char *)"--detach",
                        (char *)revision, NULL};

    if (root == NULL || url == NULL || name == NULL || revision == NULL ||
        snprintf(destination, sizeof(destination), "%s/%s", root, name) >=
            (int)sizeof(destination)) return -1;
    if (access(destination, F_OK) == 0) return 0;
    if (run_program(root, clone) == -1 || run_program(root, checkout) == -1)
        return -1;
    return 0;
}

static int configure_ds4_runtime(setup_selection *selection, const char *home,
                                 bool ssd_streaming)
{
    int written;

    if (selection == NULL || home == NULL) return -1;
    written = snprintf(selection->runtime_command,
                       sizeof(selection->runtime_command),
                       "%s/.genshell/runtimes/ds4/ds4-server --chdir "
                       "%s/.genshell/runtimes/ds4 --port 8000 --ctx 100000%s",
                       home, home, ssd_streaming ? " --ssd-streaming" : "");
    return written >= 0 &&
                   (size_t)written < sizeof(selection->runtime_command)
               ? 0 : -1;
}

static int select_ds4_model(const gsh_llm_hardware *hardware, char *quant,
                            size_t capacity, bool *ssd_streaming)
{
    static const char *const fixed_options[] = {
        "ds4f-q2 — verified for 96/128 GiB",
        "ds4f-q2-q4 — Q4 on the last six expert layers",
        "ds4f-q4 — highest quality, >=256 GiB",
    };
    const char *options[4];
    char automatic[160];
    const char *recommended;
    unsigned int requirement;
    unsigned int ram_gib;
    int choice;

    if (hardware == NULL || quant == NULL || capacity < 16U ||
        ssd_streaming == NULL) return -1;
    recommended = gsh_ds4_recommend(hardware, ssd_streaming);
    (void)snprintf(automatic, sizeof(automatic),
                   "Automatic — %s%s", recommended,
                   *ssd_streaming ? " with SSD streaming" : " resident");
    options[0] = automatic;
    options[1] = fixed_options[0];
    options[2] = fixed_options[1];
    options[3] = fixed_options[2];
    (void)printf("Recommended DS4 model: %s; build backend: %s\n",
                 automatic + sizeof("Automatic — ") - 1U,
                 gsh_ds4_build_target(hardware)[0] == '\0'
                     ? "Metal" : gsh_ds4_build_target(hardware));
    (void)puts("Choose DS4 quantization:");
    choice = arrow_menu(options, sizeof(options) / sizeof(options[0]));
    if (choice < 0 || choice > 3) return -1;
    if (choice == 0) requirement = strcmp(recommended, "ds4f-q4") == 0
                                       ? 256U : 96U;
    else requirement = choice == 1 ? 96U : (choice == 2 ? 160U : 256U);
    if (choice == 0) (void)snprintf(quant, capacity, "%s", recommended);
    else if (choice == 1) (void)snprintf(quant, capacity, "ds4f-q2");
    else if (choice == 2) (void)snprintf(quant, capacity, "ds4f-q2-q4");
    else (void)snprintf(quant, capacity, "ds4f-q4");
    ram_gib = bytes_to_gib(hardware->ram_bytes);
    *ssd_streaming = ram_gib < requirement;
    if (*ssd_streaming == true)
        (void)printf("%s exceeds the resident-memory recommendation; "
                     "SSD streaming will be enabled.\n", quant);
    return 0;
}

static int install_ds4(setup_selection *selection, const char *home,
                       const gsh_llm_hardware *hardware)
{
    static const char revision[] = "b0a147a7fba6d1a104d047d5a140e9bb4bfc13cd";
    char root[4096];
    char source[4096];
    char quant[32];
    char question[160];
    char *build[] = {(char *)"make", NULL, NULL};
    char *download[] = {(char *)"./download_model.sh", quant, NULL};
    const char *build_target;
    bool ssd_streaming;

    if (selection == NULL || home == NULL || hardware == NULL ||
        select_ds4_model(hardware, quant, sizeof(quant), &ssd_streaming) == -1 ||
        configure_ds4_runtime(selection, home, ssd_streaming) == -1)
        return -1;
    if (hardware->accelerator == GSH_LLM_ACCELERATOR_CPU ||
        !hardware->accelerator_buildable)
        (void)puts("Warning: DS4's CPU build is diagnostic; accelerated hardware "
                   "is recommended.");
    if (snprintf(question, sizeof(question),
                 "Clone, build and download %s now?", quant) >=
        (int)sizeof(question)) return -1;
    if (!yes_no(question, false))
        return 0;
    build_target = gsh_ds4_build_target(hardware);
    if (build_target[0] != '\0') build[1] = (char *)build_target;
    if (runtime_root(home, root, sizeof(root)) == -1 ||
        clone_pinned(root, "https://github.com/antirez/ds4.git", "ds4",
                     revision) == -1 ||
        snprintf(source, sizeof(source), "%s/ds4", root) >=
            (int)sizeof(source) || run_program(source, build) == -1 ||
        run_program(source, download) == -1) return -1;
    return 1;
}

static bool model_reference_valid(const char *model)
{
    size_t index;

    if (model == NULL || model[0] == '\0') return false;
    for (index = 0U; model[index] != '\0' && index < GSH_LLM_MODEL_CAP;
         index++) {
        unsigned char byte = (unsigned char)model[index];

        if (!((byte >= 'A' && byte <= 'Z') ||
              (byte >= 'a' && byte <= 'z') ||
              (byte >= '0' && byte <= '9') || strchr("._/-:", byte) != NULL))
            return false;
    }
    return index > 0U && index < GSH_LLM_MODEL_CAP;
}

static int set_llama_runtime(setup_selection *selection, const char *home,
                             const gsh_llm_hardware *hardware)
{
    const char *gpu_layers;
    int written;

    if (selection == NULL || home == NULL || hardware == NULL ||
        !model_reference_valid(selection->model)) return -1;
    gpu_layers = hardware->accelerator_buildable &&
                         hardware->accelerator != GSH_LLM_ACCELERATOR_CPU
                     ? "auto" : "0";
    written = snprintf(selection->runtime_command,
                       sizeof(selection->runtime_command),
                       "%s/.genshell/runtimes/llama.cpp/build/bin/llama-server "
                       "--host 127.0.0.1 --port 8080 --hf-repo %s --ctx-size "
                       "32768 --jinja --n-gpu-layers %s --fit on",
                       home, selection->model, gpu_layers);
    if (written < 0 || (size_t)written >= sizeof(selection->runtime_command))
        return -1;
    return 0;
}

static int select_llama_model(setup_selection *selection, const char *home,
                              const gsh_llm_hardware *hardware)
{
    const char *options[SETUP_MENU_CAP];
    char labels[SETUP_MENU_CAP][192];
    size_t recommended;
    size_t count;
    size_t index;
    int choice;

    if (selection == NULL || home == NULL || hardware == NULL) return -1;
    count = gsh_llama_model_count();
    if (count + 2U > SETUP_MENU_CAP) return -1;
    recommended = gsh_llama_recommend(hardware);
    (void)snprintf(labels[0], sizeof(labels[0]), "Automatic — %s",
                   gsh_llama_model_at(recommended)->label);
    options[0] = labels[0];
    for (index = 0U; index < count; index++) {
        const gsh_llama_model *model = gsh_llama_model_at(index);
        bool memory_fit = model->working_set_gib <=
                          gsh_llm_model_budget_gib(hardware);
        bool disk_fit = hardware->disk_free_bytes == 0U ||
                        model->download_gib + 4U <=
                            bytes_to_gib(hardware->disk_free_bytes);
        const char *fit = memory_fit && disk_fit
                              ? "fits" : (memory_fit ? "disk short"
                                                     : "oversized");
        (void)snprintf(labels[index + 1U], sizeof(labels[index + 1U]),
                       "%s — %s, ~%u GiB (%s)", model->label,
                       model->specialty, model->working_set_gib, fit);
        options[index + 1U] = labels[index + 1U];
    }
    options[count + 1U] = "Custom Hugging Face GGUF repository[:quant]";
    (void)printf("Recommended llama.cpp model: %s\n",
                 gsh_llama_model_at(recommended)->label);
    (void)puts("Choose among 10 curated llama.cpp models:");
    choice = arrow_menu(options, count + 2U);
    if (choice < 0 || choice > (int)(count + 1U)) return -1;
    if (choice == (int)(count + 1U)) {
        if (read_line("Hugging Face GGUF repository[:quant]", selection->model,
                      sizeof(selection->model), NULL) == -1)
            return -1;
    } else {
        size_t selected = choice == 0 ? recommended : (size_t)choice - 1U;
        (void)snprintf(selection->model, sizeof(selection->model), "%s",
                       gsh_llama_model_at(selected)->reference);
    }
    return set_llama_runtime(selection, home, hardware);
}

static int install_llama_cpp(setup_selection *selection, const char *home,
                             const gsh_llm_hardware *hardware)
{
    static const char revision[] = "8b4b3558f1459c13e4aa38d5c94d306a00dc6acd";
    char root[4096];
    char source[4096];
    char build_directory[4096];
    char *configure[10] = {(char *)"cmake", (char *)"-S", source,
                           (char *)"-B", build_directory,
                           (char *)"-DCMAKE_BUILD_TYPE=Release", NULL};
    char *build[] = {(char *)"cmake", (char *)"--build", build_directory,
                     (char *)"--target", (char *)"llama-server",
                     (char *)"--parallel", NULL};
    size_t configure_count = 5U;

    if (selection == NULL || home == NULL || hardware == NULL ||
        select_llama_model(selection, home, hardware) == -1)
        return -1;
    if (!yes_no("Clone and build the pinned llama.cpp runtime now?", true))
        return 0;
    if (hardware->accelerator_buildable) {
        if (hardware->accelerator == GSH_LLM_ACCELERATOR_METAL)
            configure[configure_count++] = (char *)"-DGGML_METAL=ON";
        else if (hardware->accelerator == GSH_LLM_ACCELERATOR_CUDA)
            configure[configure_count++] = (char *)"-DGGML_CUDA=ON";
        else if (hardware->accelerator == GSH_LLM_ACCELERATOR_ROCM)
            configure[configure_count++] = (char *)"-DGGML_HIP=ON";
    }
    configure[configure_count] = NULL;
    if (runtime_root(home, root, sizeof(root)) == -1 ||
        clone_pinned(root, "https://github.com/ggml-org/llama.cpp.git",
                     "llama.cpp", revision) == -1 ||
        snprintf(source, sizeof(source), "%s/llama.cpp", root) >=
            (int)sizeof(source) ||
        snprintf(build_directory, sizeof(build_directory), "%s/build",
                 source) >= (int)sizeof(build_directory) ||
        run_program(root, configure) == -1 || run_program(root, build) == -1)
        return -1;
    return 1;
}

static bool llm_config_line(const char *line, size_t length)
{
    size_t offset = 0U;

    if (line == NULL) return false;
    while (offset < length && (line[offset] == ' ' || line[offset] == '\t'))
        offset++;
    return length - offset >= 4U && memcmp(line + offset, "llm.", 4U) == 0;
}

static size_t retain_non_llm(const char *input, size_t length, char *output,
                             size_t capacity)
{
    size_t begin = 0U;
    size_t used = 0U;

    if (input == NULL || output == NULL) return 0U;
    while (begin < length) {
        size_t end = begin;
        size_t line_length;
        while (end < length && input[end] != '\n') end++;
        line_length = end - begin;
        if (!llm_config_line(input + begin, line_length)) {
            if (line_length + 1U >= capacity - used) return 0U;
            (void)memcpy(output + used, input + begin, line_length);
            used += line_length;
            output[used++] = '\n';
        }
        begin = end < length ? end + 1U : end;
    }
    output[used] = '\0';
    return used;
}

static bool append_text(char *output, size_t capacity, size_t *used,
                        const char *text)
{
    size_t length;

    if (output == NULL || used == NULL || text == NULL) return false;
    length = strlen(text);
    if (length >= capacity - *used) return false;
    (void)memcpy(output + *used, text, length + 1U);
    *used += length;
    return true;
}

static bool append_config_string(char *output, size_t capacity, size_t *used,
                                 const char *key, const char *value)
{
    size_t index;

    if (output == NULL || used == NULL || key == NULL || value == NULL)
        return false;
    if (!append_text(output, capacity, used, key) ||
        !append_text(output, capacity, used, " = \"")) return false;
    for (index = 0U; value[index] != '\0'; index++) {
        char byte = value[index];
        if ((byte == '\\' || byte == '"') &&
            !append_text(output, capacity, used, "\\")) return false;
        if (*used + 2U >= capacity) return false;
        output[(*used)++] = byte;
        output[*used] = '\0';
    }
    return append_text(output, capacity, used, "\"\n");
}

static bool append_llm_configuration(char *output, size_t capacity,
                                     size_t *used,
                                     const setup_selection *selection)
{
    char key[256];

    if (output == NULL || used == NULL || selection == NULL) return false;
    if (!append_text(output, capacity, used, "\n# gsh LLM setup\n") ||
        !append_text(output, capacity, used,
                     selection->recipe == SETUP_DISABLED
                         ? "llm.enabled = false\n" : "llm.enabled = true\n"))
        return false;
    if (selection->recipe == SETUP_DISABLED) return true;
    if (!append_config_string(output, capacity, used, "llm.default_provider",
                              selection->name) ||
        !append_text(output, capacity, used,
                     "llm.streaming = true\nllm.auto_help = true\n"
                     "llm.context.recent_exchanges = 5\n"
                     "llm.request_timeout = 120s\n") ||
        snprintf(key, sizeof(key), "llm.providers.%s.type", selection->name) >=
            (int)sizeof(key) || !append_text(output, capacity, used, key) ||
        !append_text(output, capacity, used, " = responses\n")) return false;
    (void)snprintf(key, sizeof(key), "llm.providers.%s.base_url", selection->name);
    if (!append_config_string(output, capacity, used, key, selection->base_url))
        return false;
    (void)snprintf(key, sizeof(key), "llm.providers.%s.model", selection->name);
    if (!append_config_string(output, capacity, used, key, selection->model))
        return false;
    (void)snprintf(key, sizeof(key), "llm.providers.%s.credential", selection->name);
    if (!append_config_string(output, capacity, used, key,
                              selection->credential)) return false;
    (void)snprintf(key, sizeof(key), "llm.providers.%s.runtime.managed", selection->name);
    if (!append_text(output, capacity, used, key) ||
        !append_text(output, capacity, used,
                     selection->managed ? " = true\n" : " = false\n"))
        return false;
    (void)snprintf(key, sizeof(key),
                   "llm.providers.%s.runtime.idle_timeout", selection->name);
    if (!append_text(output, capacity, used, key) ||
        !append_text(output, capacity, used,
                     selection->managed ? " = 5m\n" : " = off\n"))
        return false;
    (void)snprintf(key, sizeof(key), "llm.providers.%s.runtime.command", selection->name);
    return append_config_string(output, capacity, used, key,
                                selection->runtime_command);
}

static ssize_t read_file(int descriptor, char *output, size_t capacity)
{
    size_t used = 0U;

    if (descriptor < 0 || output == NULL || capacity < 2U) return -1;
    while (used + 1U < capacity) {
        ssize_t count = read(descriptor, output + used, capacity - used - 1U);
        if (count > 0) used += (size_t)count;
        else if (count == 0) { output[used] = '\0'; return (ssize_t)used; }
        else if (errno != EINTR) return -1;
    }
    errno = EFBIG;
    return -1;
}

static int replace_configuration(const char *home,
                                 const setup_selection *selection)
{
    char source[SETUP_FILE_CAP + 1U];
    char output[SETUP_FILE_CAP + 1U];
    char path[4096];
    char temporary[4096];
    ssize_t length;
    size_t used;
    int input;
    int target;
    int result = -1;

    if (home == NULL || selection == NULL ||
        snprintf(path, sizeof(path), "%s/.gshrc", home) >= (int)sizeof(path) ||
        snprintf(temporary, sizeof(temporary), "%s/.gshrc.tmp.%ld", home,
                 (long)getpid()) >= (int)sizeof(temporary)) return -1;
    input = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (input < 0) return -1;
    length = read_file(input, source, sizeof(source));
    (void)close(input);
    if (length < 0) return -1;
    used = retain_non_llm(source, (size_t)length, output, sizeof(output));
    if (used == 0U ||
        !append_llm_configuration(output, sizeof(output), &used, selection))
        return -1;
    target = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC |
                             O_NOFOLLOW, 0600);
    if (target < 0) return -1;
    if (write_all(target, output, used) == 0 && fsync(target) == 0 &&
        close(target) == 0 && rename(temporary, path) == 0) result = 0;
    else (void)close(target);
    if (result == -1) (void)unlink(temporary);
    return result;
}

static int initialize_configuration(const char *home)
{
    gsh_shell_config config;

    if (home == NULL) return -1;
    gsh_config_defaults(&config);
    return gsh_config_load(&config, home, true);
}

static bool local_runtime_installed(setup_recipe recipe, const char *home)
{
    char path[4096];
    const char *suffix;

    if (home == NULL) return false;
    if (recipe == SETUP_DS4)
        suffix = "/.genshell/runtimes/ds4/ds4-server";
    else if (recipe == SETUP_LLAMA_CPP)
        suffix = "/.genshell/runtimes/llama.cpp/build/bin/llama-server";
    else return false;
    if (snprintf(path, sizeof(path), "%s%s", home, suffix) >=
        (int)sizeof(path)) return false;
    return access(path, X_OK) == 0;
}

int main(void)
{
    static const char *const options[] = {
        "antirez/ds4 (local, port 8000)",
        "llama.cpp (local, port 8080)",
        "Custom Responses API endpoint",
        "Disable LLM integration",
    };
    gsh_llm_hardware hardware;
    setup_selection selection;
    const char *home = getenv("HOME");
    int choice;
    int installed = 0;
    bool available;
    bool local_installation;

    if (home == NULL || home[0] != '/' || initialize_configuration(home) == -1) {
        (void)fprintf(stderr, "gsh setup: cannot initialize ~/.gshrc: %s\n",
                      strerror(errno));
        return 1;
    }
    if (gsh_llm_hardware_detect(&hardware, home) == -1) {
        (void)fputs("gsh setup: cannot detect local hardware\n", stderr);
        return 1;
    }
    (void)puts("gsh inference server\nUse arrow keys and Enter.");
    print_hardware(&hardware);
    choice = arrow_menu(options, sizeof(options) / sizeof(options[0]));
    if (choice < 0 || choice > SETUP_DISABLED) {
        (void)fputs("gsh setup: invalid selection\n", stderr);
        return 1;
    }
    configure_recipe(&selection, (setup_recipe)choice, home);
    local_installation = local_runtime_installed((setup_recipe)choice, home);
    if (choice == SETUP_CUSTOM && configure_custom(&selection) == -1) {
        (void)fputs("gsh setup: invalid custom provider\n", stderr);
        return 1;
    }
    available = choice != SETUP_DISABLED && endpoint_available(&selection);
    if (available) (void)puts("Existing Responses API server detected; using it.");
    else if (choice == SETUP_DS4)
        installed = install_ds4(&selection, home, &hardware);
    else if (choice == SETUP_LLAMA_CPP)
        installed = install_llama_cpp(&selection, home, &hardware);
    if (installed < 0) {
        (void)fprintf(stderr, "gsh setup: %s installation failed\n",
                      choice == SETUP_DS4 ? "DS4" : "llama.cpp");
        return 1;
    }
    selection.managed = installed > 0 || local_installation;
    if (replace_configuration(home, &selection) == -1) {
        (void)fprintf(stderr, "gsh setup: cannot update ~/.gshrc: %s\n",
                      strerror(errno));
        return 1;
    }
    (void)puts("gsh setup: configuration saved in ~/.gshrc");
    if (selection.managed && !available)
        (void)puts("The runtime will start automatically with the first AI "
                   "request.");
    else if (!available && choice != SETUP_DISABLED)
        (void)puts("No runtime was installed. Start a compatible server before "
                   "using '?'.");
    return 0;
}
