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

#ifndef __NIKSS_COUNTER_H
#define __NIKSS_COUNTER_H

#include <nikss/nikss.h>

/* Might be used to test whether given type ID is a valid counter */
nikss_counter_type_t get_counter_type(nikss_btf_t *btf, uint32_t type_id);

int convert_counter_entry_to_data(nikss_counter_context_t *ctx, nikss_counter_entry_t *entry, char *buffer);
int convert_counter_data_to_entry(const char *data, size_t counter_size,
                                  nikss_counter_type_t counter_type, nikss_counter_entry_t *entry);

#endif  /* __NIKSS_COUNTER_H */
