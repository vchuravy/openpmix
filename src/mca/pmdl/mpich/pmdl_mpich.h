/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 *
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PMIX_PMDL_MPICH_H
#define PMIX_PMDL_MPICH_H

#include "src/include/pmix_config.h"

#include "src/mca/pmdl/pmdl.h"

BEGIN_C_DECLS

typedef struct {
    pmix_pmdl_base_component_t super;
    char *incparms;
    char *excparms;
    char **include;
    char **exclude;
} pmix_pmdl_mpich_component_t;

/* the component must be visible data for the linker to find it */
PMIX_EXPORT extern pmix_pmdl_mpich_component_t mca_pmdl_mpich_component;
extern pmix_pmdl_module_t pmix_pmdl_mpich_module;

END_C_DECLS

#endif
