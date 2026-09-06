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

#include <dirent.h> /* CANON-INCLUDE: linux */
#include <errno.h>
#include <fcntl.h>
#include <limits.h> /* CANON-INCLUDE: linux */
#include <stdio.h>
#include <stdlib.h> /* CANON-INCLUDE: linux */
#include <string.h>
#if defined(__APPLE__)
#include <sys/sysctl.h> /* CANON-INCLUDE: macos */
#endif
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <sys/wait.h> /* CANON-INCLUDE: linux */
#include <unistd.h>

#define require(condition) (condition)

enum {
    HARDWARE_CAPTURE_CAP = 16384,
    HARDWARE_COMMAND_CAP = 4096,
};

static const gsh_llama_model LLAMA_MODELS[] = {
    {"Qwen3 Coder Next 80B Q8", "ggml-org/Qwen3-Coder-Next-GGUF:Q8_0",
     "coding and tool use", 80U, 96U},
    {"Qwen3.8 27B Q4_K_M", "ggml-org/Qwen3.8-27B-GGUF:Q4_K_M",
     "general reasoning", 18U, 24U},
    {"Qwen3.6 35B-A3B Q4_K_M", "ggml-org/Qwen3.6-35B-A3B-GGUF:Q4_K_M",
     "fast MoE reasoning", 20U, 26U},
    {"GLM-4.7 Flash Q8", "ggml-org/GLM-4.7-Flash-GGUF:Q8_0",
     "agentic coding", 30U, 40U},
    {"gpt-oss 20B MXFP4", "ggml-org/gpt-oss-20b-GGUF:MXFP4",
     "tool use and reasoning", 12U, 16U},
    {"Nemotron 3.5 Lightning 30B-A3B Q8",
     "ggml-org/NVIDIA-Nemotron-3.5-Lightning-30B-A3B-GGUF:Q8_0",
     "efficient reasoning", 32U, 40U},
    {"Gemma 4 12B IT Q8", "ggml-org/gemma-4-12B-it-GGUF:Q8_0",
     "balanced assistant", 12U, 16U},
    {"Gemma 3 12B IT Q4_K_M", "ggml-org/gemma-3-12b-it-GGUF:Q4_K_M",
     "compact assistant", 7U, 10U},
    {"Gemma 3 4B IT Q4_K_M", "ggml-org/gemma-3-4b-it-GGUF:Q4_K_M",
     "low-memory assistant", 3U, 6U},
    {"Qwen3.5 0.8B Q8", "ggml-org/Qwen3.5-0.8B-GGUF:Q8_0",
     "minimal-memory assistant", 1U, 3U},
};

static int capture_program(char *const arguments[], char *output,
                           size_t capacity)
{
    int channel[2] = {-1, -1};
    int status = 0;
    pid_t child;
    size_t used = 0U;
    unsigned int reads;

    if (!require(arguments != NULL && arguments[0] != NULL)) return -1;
    if (!require(output != NULL && capacity > 1U)) return -1;
    if (pipe(channel) == -1) return -1;
    child = fork();
    if (child == 0) {
        int null_descriptor = open("/dev/null", O_WRONLY | O_CLOEXEC);
        (void)close(channel[0]);
        if (dup2(channel[1], STDOUT_FILENO) == -1) _exit(125);
        if (null_descriptor >= 0)
            (void)dup2(null_descriptor, STDERR_FILENO);
        (void)close(channel[1]);
        if (null_descriptor >= 0) (void)close(null_descriptor);
        execvp(arguments[0], arguments);
        _exit(127);
    }
    (void)close(channel[1]);
    if (child < 0) { (void)close(channel[0]); return -1; }
    for (reads = 0U; reads < HARDWARE_COMMAND_CAP; reads++) {
        char chunk[4096];
        ssize_t count = read(channel[0], chunk, sizeof(chunk));
        if (count > 0) {
            size_t available = capacity - used - 1U;
            size_t retained = (size_t)count < available
                                  ? (size_t)count : available;
            if (retained > 0U) {
                (void)memcpy(output + used, chunk, retained);
                used += retained;
            }
        }
        else if (count == -1 && errno == EINTR) reads--;
        else break;
    }
    output[used] = '\0';
    (void)close(channel[0]);
    while (waitpid(child, &status, 0) == -1 && errno == EINTR) {
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

static uint64_t total_memory(void)
{
#if defined(__APPLE__)
    uint64_t bytes = 0U;
    size_t length = sizeof(bytes);

    if (sysctlbyname("hw.memsize", &bytes, &length, NULL, 0U) == 0 &&
        length == sizeof(bytes)) return bytes;
#endif
    {
        long pages = sysconf(_SC_PHYS_PAGES);
        long page_size = sysconf(_SC_PAGESIZE);

        if (pages > 0 && page_size > 0 &&
            (uint64_t)pages <= UINT64_MAX / (uint64_t)page_size)
            return (uint64_t)pages * (uint64_t)page_size;
    }
    return 0U;
}

static void detect_identity(gsh_llm_hardware *hardware)
{
    struct utsname identity;

    if (!require(hardware != NULL)) return;
    if (uname(&identity) == 0) {
        (void)snprintf(hardware->operating_system,
                       sizeof(hardware->operating_system), "%.*s",
                       (int)sizeof(hardware->operating_system) - 1,
                       identity.sysname);
        (void)snprintf(hardware->architecture,
                       sizeof(hardware->architecture), "%.*s",
                       (int)sizeof(hardware->architecture) - 1,
                       identity.machine);
    } else {
        (void)memcpy(hardware->operating_system, "unknown", sizeof("unknown"));
        (void)memcpy(hardware->architecture, "unknown", sizeof("unknown"));
    }
}

#if defined(__APPLE__)
static void detect_apple(gsh_llm_hardware *hardware)
{
    size_t length;

    if (!require(hardware != NULL)) return;
    if (strcmp(hardware->architecture, "arm64") != 0) return;
    hardware->accelerator = GSH_LLM_ACCELERATOR_METAL;
    hardware->accelerator_buildable = true;
    hardware->accelerator_bytes = hardware->ram_bytes;
    length = sizeof(hardware->device);
    if (sysctlbyname("machdep.cpu.brand_string", hardware->device, &length,
                     NULL, 0U) != 0 || hardware->device[0] == '\0')
        (void)memcpy(hardware->device, "Apple Silicon",
                     sizeof("Apple Silicon"));
    hardware->device[sizeof(hardware->device) - 1U] = '\0';
}
#endif

static uint64_t parse_nvidia_output(gsh_llm_hardware *hardware,
                                    const char *output)
{
    const char *line = output;
    uint64_t total_mib = 0U;
    unsigned int devices = 0U;

    if (!require(hardware != NULL && output != NULL)) return 0U;
    while (*line != '\0' && devices < 64U) {
        const char *comma = strchr(line, ',');
        const char *end = strchr(line, '\n');
        char *number_end = NULL;
        unsigned long long mib;

        if (end == NULL) end = line + strlen(line);
        if (comma == NULL || comma >= end) break;
        if (devices == 0U) {
            size_t name_length = (size_t)(comma - line);
            while (name_length > 0U && line[name_length - 1U] == ' ')
                name_length--;
            if (name_length >= sizeof(hardware->device))
                name_length = sizeof(hardware->device) - 1U;
            (void)memcpy(hardware->device, line, name_length);
            hardware->device[name_length] = '\0';
        }
        errno = 0;
        mib = strtoull(comma + 1U, &number_end, 10);
        if (errno != 0 || number_end == comma + 1U ||
            mib > (UINT64_MAX / 1048576U) - total_mib)
            break;
        total_mib += (uint64_t)mib;
        devices++;
        line = *end == '\0' ? end : end + 1U;
    }
    hardware->dgx_spark = strstr(output, "GB10") != NULL ||
                          strstr(output, "DGX Spark") != NULL;
    return total_mib * 1048576U;
}

static bool detect_nvidia(gsh_llm_hardware *hardware)
{
    char output[HARDWARE_CAPTURE_CAP];
    char *query[] = {(char *)"nvidia-smi",
                     (char *)"--query-gpu=name,memory.total",
                     (char *)"--format=csv,noheader,nounits", NULL};
    char *toolkit[] = {(char *)"nvcc", (char *)"--version", NULL};

    if (!require(hardware != NULL)) return false;
    if (capture_program(query, output, sizeof(output)) == -1) return false;
    hardware->accelerator = GSH_LLM_ACCELERATOR_CUDA;
    hardware->accelerator_bytes = parse_nvidia_output(hardware, output);
    hardware->accelerator_buildable =
        capture_program(toolkit, output, sizeof(output)) == 0;
    return true;
}

static uint64_t read_unsigned_file(const char *path)
{
    char data[64];
    char *end = NULL;
    unsigned long long value;
    ssize_t count;
    int descriptor;

    if (!require(path != NULL)) return 0U;
    descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) return 0U;
    count = read(descriptor, data, sizeof(data) - 1U);
    (void)close(descriptor);
    if (count <= 0) return 0U;
    data[count] = '\0';
    errno = 0;
    value = strtoull(data, &end, 10);
    return errno == 0 && end != data ? (uint64_t)value : 0U;
}

static uint64_t linux_drm_memory(void)
{
    DIR *directory;
    struct dirent *entry;
    uint64_t total = 0U;
    unsigned int cards = 0U;

    directory = opendir("/sys/class/drm");
    if (directory == NULL) return 0U;
    while ((entry = readdir(directory)) != NULL && cards < 64U) {
        char path[512];
        uint64_t bytes;

        if (strncmp(entry->d_name, "card", 4U) != 0 ||
            entry->d_name[4] < '0' || entry->d_name[4] > '9' ||
            strchr(entry->d_name, '-') != NULL) continue;
        if (snprintf(path, sizeof(path),
                     "/sys/class/drm/%s/device/mem_info_vram_total",
                     entry->d_name) >= (int)sizeof(path)) continue;
        bytes = read_unsigned_file(path);
        if (bytes <= UINT64_MAX - total) total += bytes;
        cards++;
    }
    (void)closedir(directory);
    return total;
}

static bool detect_rocm(gsh_llm_hardware *hardware)
{
    char output[HARDWARE_CAPTURE_CAP];
    char *query[] = {(char *)"rocminfo", NULL};
    char *toolkit[] = {(char *)"hipconfig", (char *)"--version", NULL};

    if (!require(hardware != NULL)) return false;
    if (capture_program(query, output, sizeof(output)) == -1) return false;
    hardware->accelerator = GSH_LLM_ACCELERATOR_ROCM;
    hardware->accelerator_bytes = linux_drm_memory();
    hardware->strix_halo = strstr(output, "gfx1151") != NULL ||
                            strstr(output, "Strix Halo") != NULL;
    if (hardware->strix_halo && hardware->accelerator_bytes == 0U)
        hardware->accelerator_bytes = hardware->ram_bytes;
    hardware->accelerator_buildable =
        capture_program(toolkit, output, sizeof(output)) == 0;
    (void)memcpy(hardware->device,
                 hardware->strix_halo ? "AMD Strix Halo" : "AMD ROCm GPU",
                 hardware->strix_halo ? sizeof("AMD Strix Halo")
                                       : sizeof("AMD ROCm GPU"));
    return true;
}

int gsh_llm_hardware_detect(gsh_llm_hardware *hardware, const char *home)
{
    struct statvfs storage;
    long cpus;

    if (!require(hardware != NULL)) return -1;
    if (!require(home != NULL && home[0] == '/')) return -1;
    (void)memset(hardware, 0, sizeof(*hardware));
    hardware->accelerator = GSH_LLM_ACCELERATOR_CPU;
    (void)memcpy(hardware->device, "CPU", sizeof("CPU"));
    hardware->ram_bytes = total_memory();
    cpus = sysconf(_SC_NPROCESSORS_ONLN);
    hardware->logical_cpus = cpus > 0 ? (unsigned int)cpus : 1U;
    detect_identity(hardware);
    if (statvfs(home, &storage) == 0 && storage.f_frsize > 0U &&
        (uint64_t)storage.f_bavail <= UINT64_MAX / (uint64_t)storage.f_frsize)
        hardware->disk_free_bytes =
            (uint64_t)storage.f_bavail * (uint64_t)storage.f_frsize;
#if defined(__APPLE__)
    detect_apple(hardware);
#endif
    if (hardware->accelerator == GSH_LLM_ACCELERATOR_CPU &&
        !detect_nvidia(hardware))
        (void)detect_rocm(hardware);
    return hardware->ram_bytes > 0U ? 0 : -1;
}

const char *gsh_llm_accelerator_name(gsh_llm_accelerator accelerator)
{
    if (accelerator == GSH_LLM_ACCELERATOR_METAL) return "Metal";
    if (accelerator == GSH_LLM_ACCELERATOR_CUDA) return "CUDA";
    if (accelerator == GSH_LLM_ACCELERATOR_ROCM) return "ROCm";
    return "CPU";
}

unsigned int gsh_llm_model_budget_gib(const gsh_llm_hardware *hardware)
{
    uint64_t bytes;
    uint64_t gib = 1073741824U;

    if (!require(hardware != NULL)) return 0U;
    bytes = hardware->ram_bytes - hardware->ram_bytes / 4U;
    if (!hardware->accelerator_buildable ||
        hardware->accelerator == GSH_LLM_ACCELERATOR_CPU) {
        uint64_t cpu_limit = 10U * gib;
        bytes = hardware->ram_bytes / 2U;
        if (bytes > cpu_limit) bytes = cpu_limit;
    } else if (hardware->accelerator != GSH_LLM_ACCELERATOR_METAL &&
               hardware->accelerator_bytes > 0U) {
        uint64_t device_bytes = hardware->accelerator_bytes -
                                hardware->accelerator_bytes / 10U;
        if (bytes > device_bytes) bytes = device_bytes;
    }
    return bytes / gib > UINT_MAX ? UINT_MAX : (unsigned int)(bytes / gib);
}

size_t gsh_llama_model_count(void)
{
    return sizeof(LLAMA_MODELS) / sizeof(LLAMA_MODELS[0]);
}

const gsh_llama_model *gsh_llama_model_at(size_t index)
{
    if (!require(index < gsh_llama_model_count())) return NULL;
    return &LLAMA_MODELS[index];
}

size_t gsh_llama_recommend(const gsh_llm_hardware *hardware)
{
    unsigned int budget;
    uint64_t disk;
    size_t index;

    if (!require(hardware != NULL)) return gsh_llama_model_count() - 1U;
    budget = gsh_llm_model_budget_gib(hardware);
    disk = hardware->disk_free_bytes / 1073741824U;
    for (index = 0U; index < gsh_llama_model_count(); index++) {
        const gsh_llama_model *model = &LLAMA_MODELS[index];
        if (model->working_set_gib <= budget &&
            (disk == 0U || model->download_gib + 4U <= disk)) return index;
    }
    return gsh_llama_model_count() - 1U;
}

const char *gsh_ds4_recommend(const gsh_llm_hardware *hardware,
                              bool *ssd_streaming)
{
    uint64_t ram_gib;

    if (!require(hardware != NULL && ssd_streaming != NULL)) return "ds4f-q2";
    ram_gib = hardware->ram_bytes / 1073741824U;
    *ssd_streaming = ram_gib < 96U;
    return ram_gib >= 256U ? "ds4f-q4" : "ds4f-q2";
}

const char *gsh_ds4_build_target(const gsh_llm_hardware *hardware)
{
    if (!require(hardware != NULL)) return "cpu";
    if (!hardware->accelerator_buildable) return "cpu";
    if (hardware->accelerator == GSH_LLM_ACCELERATOR_METAL) return "";
    if (hardware->accelerator == GSH_LLM_ACCELERATOR_CUDA)
        return hardware->dgx_spark ? "cuda-spark" : "cuda-generic";
    if (hardware->accelerator == GSH_LLM_ACCELERATOR_ROCM &&
        hardware->strix_halo) return "strix-halo";
    return "cpu";
}
