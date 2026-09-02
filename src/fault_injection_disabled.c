#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif
#if _POSIX_C_SOURCE < 202405L
#error "gsh requires the POSIX.1-2024 feature-test baseline"
#endif

#include "fault_injection.h"

#include <stddef.h>

void gsh_fault_initialize(void)
{
}

bool gsh_fault_should_fail(gsh_fault_point point, int error)
{
    (void)point;
    (void)error;
    return false;
}

bool gsh_fault_active(void)
{
    return false;
}

bool gsh_fault_selected(gsh_fault_point point)
{
    (void)point;
    return false;
}

unsigned long gsh_fault_trigger(void)
{
    return 1U;
}

unsigned long *gsh_fault_counter(void)
{
    return NULL;
}
