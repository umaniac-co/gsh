#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif
#if _POSIX_C_SOURCE < 202405L
#error "gsh requires the POSIX.1-2024 feature-test baseline"
#endif

#include "../src/llm_hardware.h"

#include <stdio.h>
#include <string.h>

static gsh_llm_hardware profile(unsigned int ram_gib,
                                unsigned int accelerator_gib,
                                unsigned int disk_gib,
                                gsh_llm_accelerator accelerator,
                                bool buildable)
{
    gsh_llm_hardware hardware;

    (void)memset(&hardware, 0, sizeof(hardware));
    hardware.ram_bytes = (uint64_t)ram_gib * 1073741824U;
    hardware.accelerator_bytes = (uint64_t)accelerator_gib * 1073741824U;
    hardware.disk_free_bytes = (uint64_t)disk_gib * 1073741824U;
    hardware.accelerator = accelerator;
    hardware.accelerator_buildable = buildable;
    return hardware;
}

static int recommendation_cases(void)
{
    gsh_llm_hardware metal128 =
        profile(128U, 128U, 1000U, GSH_LLM_ACCELERATOR_METAL, true);
    gsh_llm_hardware metal32 =
        profile(32U, 32U, 200U, GSH_LLM_ACCELERATOR_METAL, true);
    gsh_llm_hardware cuda16 =
        profile(64U, 16U, 200U, GSH_LLM_ACCELERATOR_CUDA, true);
    gsh_llm_hardware cpu64 =
        profile(64U, 0U, 200U, GSH_LLM_ACCELERATOR_CPU, false);

    if (gsh_llama_recommend(&metal128) != 0U ||
        gsh_llama_recommend(&metal32) != 1U ||
        strcmp(gsh_llama_model_at(gsh_llama_recommend(&cuda16))->label,
               "Gemma 3 12B IT Q4_K_M") != 0 ||
        gsh_llm_model_budget_gib(&cpu64) != 10U) return 1;
    return gsh_llama_model_count() != 10U;
}

static int ds4_cases(void)
{
    gsh_llm_hardware metal128 =
        profile(128U, 128U, 1000U, GSH_LLM_ACCELERATOR_METAL, true);
    gsh_llm_hardware cuda512 =
        profile(512U, 128U, 1000U, GSH_LLM_ACCELERATOR_CUDA, true);
    gsh_llm_hardware small =
        profile(64U, 64U, 500U, GSH_LLM_ACCELERATOR_METAL, true);
    bool streaming = false;

    if (strcmp(gsh_ds4_recommend(&metal128, &streaming), "ds4f-q2") != 0 ||
        streaming || strcmp(gsh_ds4_build_target(&metal128), "") != 0)
        return 1;
    if (strcmp(gsh_ds4_recommend(&cuda512, &streaming), "ds4f-q4") != 0 ||
        streaming || strcmp(gsh_ds4_build_target(&cuda512),
                            "cuda-generic") != 0) return 1;
    if (strcmp(gsh_ds4_recommend(&small, &streaming), "ds4f-q2") != 0 ||
        !streaming) return 1;
    return 0;
}

int main(void)
{
    gsh_llm_hardware detected;

    if (recommendation_cases() != 0 || ds4_cases() != 0 ||
        gsh_llm_hardware_detect(&detected, "/") == -1 ||
        detected.ram_bytes == 0U || detected.logical_cpus == 0U) {
        (void)fputs("llm hardware: failed\n", stderr);
        return 1;
    }
    (void)puts("llm hardware: detection and recommendations passed");
    return 0;
}
