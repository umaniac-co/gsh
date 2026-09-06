#ifndef GSH_LLM_HARDWARE_H
#define GSH_LLM_HARDWARE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    GSH_LLM_ACCELERATOR_CPU = 0,
    GSH_LLM_ACCELERATOR_METAL,
    GSH_LLM_ACCELERATOR_CUDA,
    GSH_LLM_ACCELERATOR_ROCM,
} gsh_llm_accelerator;

typedef struct {
    char operating_system[32];
    char architecture[32];
    char device[128];
    uint64_t ram_bytes;
    uint64_t accelerator_bytes;
    uint64_t disk_free_bytes;
    unsigned int logical_cpus;
    gsh_llm_accelerator accelerator;
    bool accelerator_buildable;
    bool dgx_spark;
    bool strix_halo;
} gsh_llm_hardware;

typedef struct {
    const char *label;
    const char *reference;
    const char *specialty;
    unsigned int download_gib;
    unsigned int working_set_gib;
} gsh_llama_model;

int gsh_llm_hardware_detect(gsh_llm_hardware *hardware, const char *home);
const char *gsh_llm_accelerator_name(gsh_llm_accelerator accelerator);
unsigned int gsh_llm_model_budget_gib(const gsh_llm_hardware *hardware);
size_t gsh_llama_model_count(void);
const gsh_llama_model *gsh_llama_model_at(size_t index);
size_t gsh_llama_recommend(const gsh_llm_hardware *hardware);
const char *gsh_ds4_recommend(const gsh_llm_hardware *hardware,
                              bool *ssd_streaming);
const char *gsh_ds4_build_target(const gsh_llm_hardware *hardware);

#endif
