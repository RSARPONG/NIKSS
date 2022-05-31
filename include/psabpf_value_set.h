/*
 * Copyright 2022 Orange
 * Copyright 2022 Warsaw University of Technology
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __PSABPF_VALUE_SET_H_
#define __PSABPF_VALUE_SET_H_

#include <psabpf.h>

typedef struct psabpf_value_set {
    psabpf_struct_field_set_t value;
    void *raw_data;
    size_t current_field_id;
    psabpf_struct_field_t current;
} psabpf_value_set_t;

typedef struct psabpf_value_set_context {
    psabpf_bpf_map_descriptor_t set_map;
    psabpf_btf_t btf_metadata;
    psabpf_struct_field_descriptor_set_t fds;
    psabpf_value_set_t current_value;
    void *prev_entry_key;
} psabpf_value_set_context_t;

void psabpf_value_set_context_init(psabpf_value_set_context_t *ctx);
void psabpf_value_set_context_free(psabpf_value_set_context_t *ctx);
int psabpf_value_set_context_name(psabpf_context_t *psabpf_ctx, psabpf_value_set_context_t *ctx, const char *name);

void psabpf_value_set_init(psabpf_value_set_t *value);
void psabpf_value_set_free(psabpf_value_set_t *value);
psabpf_value_set_t * psabpf_value_set_get_next(psabpf_value_set_context_t *ctx);

int psabpf_value_set_set_value(psabpf_value_set_t *value, const void *data, size_t data_len);
psabpf_struct_field_t * psabpf_value_set_get_next_value_field(psabpf_value_set_context_t *ctx, psabpf_value_set_t *entry);

int psabpf_value_set_insert(psabpf_value_set_context_t *ctx, psabpf_value_set_t *value);
int psabpf_value_set_delete(psabpf_value_set_context_t *ctx, psabpf_value_set_t *value);

#endif /* __PSABPF_VALUE_SET_H_ */
