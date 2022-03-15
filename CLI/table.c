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

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>

#include <jansson.h>

#include <psabpf.h>
#include "table.h"
#include "common.h"
#include "counter.h"
#include "meter.h"

/******************************************************************************
 * Command line parsing functions
 *****************************************************************************/

static int parse_dst_table(int *argc, char ***argv, psabpf_context_t *psabpf_ctx,
                           psabpf_table_entry_ctx_t *ctx, const char **table_name, bool can_be_last)
{
    if (is_keyword(**argv, "id")) {
        NEXT_ARGP_RET();
        fprintf(stderr, "id: table access not supported\n");
        return ENOTSUP;
    } else if (is_keyword(**argv, "name")) {
        NEXT_ARGP_RET();
        fprintf(stderr, "name: table access not supported yet\n");
        return ENOTSUP;
    } else {
        if (table_name != NULL)
            *table_name = **argv;
        int error_code = psabpf_table_entry_ctx_tblname(psabpf_ctx, ctx, **argv);
        if (error_code != NO_ERROR)
            return error_code;
    }

    if (can_be_last) {
        NEXT_ARGP();
    } else {
        NEXT_ARGP_RET();
    }

    return NO_ERROR;
}

static int parse_table_action(int *argc, char ***argv, psabpf_table_entry_ctx_t *ctx,
                              psabpf_action_t *action, bool *indirect_table, bool can_be_last)
{
    *indirect_table = false;

    if (is_keyword(**argv, "id")) {
        NEXT_ARGP_RET();
        char *ptr;
        psabpf_action_set_id(action, strtoul(**argv, &ptr, 0));
        if (*ptr) {
            fprintf(stderr, "%s: unable to parse as an action id\n", **argv);
            return EINVAL;
        }
    } else if (is_keyword(**argv, "ref")) {
        *indirect_table = true;
        psabpf_table_entry_ctx_mark_indirect(ctx);
    } else {
        fprintf(stderr, "specify an action by name is not supported yet\n");
        return ENOTSUP;
    }
    if (can_be_last)
        NEXT_ARGP();
    else
        NEXT_ARGP_RET();

    return NO_ERROR;
}

static int parse_table_key(int *argc, char ***argv, psabpf_table_entry_t *entry)
{
    bool has_any_key = false;
    int error_code = EPERM;

    if (!is_keyword(**argv, "key"))
        return NO_ERROR;

    do {
        NEXT_ARGP_RET();
        if (is_keyword(**argv, "data") || is_keyword(**argv, "priority"))
            return NO_ERROR;

        if (is_keyword(**argv, "none")) {
            if (!has_any_key) {
                NEXT_ARGP();
                return NO_ERROR;
            } else {
                fprintf(stderr, "Unexpected none key\n");
                return EPERM;
            }
        }

        psabpf_match_key_t mk;
        psabpf_matchkey_init(&mk);
        char *substr_ptr;
        if ((substr_ptr = strstr(**argv, "/")) != NULL) {
            psabpf_matchkey_type(&mk, PSABPF_LPM);
            *(substr_ptr++) = 0;
            if (*substr_ptr == 0) {
                fprintf(stderr, "missing prefix length for LPM key\n");
                return EINVAL;
            }
            error_code = translate_data_to_bytes(**argv, &mk, CTX_MATCH_KEY);
            if (error_code != NO_ERROR)
                return error_code;
            char *ptr;
            psabpf_matchkey_prefix(&mk, strtoul(substr_ptr, &ptr, 0));
            if (*ptr) {
                fprintf(stderr, "%s: unable to parse prefix length\n", substr_ptr);
                return EINVAL;
            }
        } else if (strstr(**argv, "..") != NULL) {
            fprintf(stderr, "range match key not supported yet\n");
            return ENOTSUP;
        } else if ((substr_ptr = strstr(**argv, "^")) != NULL) {
            psabpf_matchkey_type(&mk, PSABPF_TERNARY);
            /* Split data and mask */
            *substr_ptr = 0;
            substr_ptr++;
            if (*substr_ptr == 0) {
                fprintf(stderr, "missing mask for ternary key\n");
                return EINVAL;
            }
            error_code = translate_data_to_bytes(**argv, &mk, CTX_MATCH_KEY);
            if (error_code != NO_ERROR)
                return error_code;
            error_code = translate_data_to_bytes(substr_ptr, &mk, CTX_MATCH_KEY_TERNARY_MASK);
            if (error_code != NO_ERROR)
                return error_code;
        } else {
            psabpf_matchkey_type(&mk, PSABPF_EXACT);
            error_code = translate_data_to_bytes(**argv, &mk, CTX_MATCH_KEY);
            if (error_code != NO_ERROR)
                return error_code;
        }
        error_code = psabpf_table_entry_matchkey(entry, &mk);
        psabpf_matchkey_free(&mk);
        if (error_code != NO_ERROR)
            return error_code;

        has_any_key = true;
    } while ((*argc) > 1);
    NEXT_ARGP();

    return NO_ERROR;
}

static int parse_direct_counter_entry(int *argc, char ***argv,
                                      psabpf_table_entry_ctx_t *ctx, psabpf_table_entry_t *entry,
                                      psabpf_direct_counter_context_t *dc, psabpf_counter_entry_t *counter)
{
    if (!is_keyword(**argv, "counter"))
        return EINVAL;

    NEXT_ARGP_RET();
    const char *name = **argv;

    int ret = psabpf_direct_counter_ctx_name(dc, ctx, name);
    if (ret != NO_ERROR) {
        fprintf(stderr, "%s: DirectCounter not found\n", name);
        return ret;
    }

    NEXT_ARGP_RET();
    ret = parse_counter_value_str(**argv, psabpf_direct_counter_get_type(dc), counter);
    if (ret != NO_ERROR)
        return ret;

    ret = psabpf_table_entry_set_direct_counter(entry, dc, counter);
    if (ret != NO_ERROR)
        fprintf(stderr, "%s: failed to append DirectCounter to table entry\n", name);

    return ret;
}

static int parse_direct_meter_entry(int *argc, char ***argv,
                                    psabpf_table_entry_ctx_t *ctx, psabpf_table_entry_t *entry,
                                    psabpf_direct_meter_context_t *dm, psabpf_meter_entry_t *meter)
{
    if (!is_keyword(**argv, "meter"))
        return EINVAL;

    NEXT_ARGP_RET();
    const char *meter_name = **argv;

    int ret = psabpf_direct_meter_ctx_name(dm, ctx, meter_name);
    if (ret != NO_ERROR) {
        fprintf(stderr, "%s: DirectMeter not found\n", meter_name);
        return ret;
    }

    ret = parse_meter_data(argc, argv, meter);
    if (ret != NO_ERROR)
        return ret;

    ret = psabpf_table_entry_set_direct_meter(entry, dm, meter);
    if (ret != NO_ERROR)
        fprintf(stderr, "%s: failed to append DirectMeter to table entry\n", meter_name);

    return ret;
}

static int parse_action_data(int *argc, char ***argv, psabpf_table_entry_ctx_t *ctx,
                             psabpf_table_entry_t *entry, psabpf_action_t *action, bool indirect_table)
{
    if (!is_keyword(**argv, "data")) {
        if (indirect_table) {
            fprintf(stderr, "expected action reference\n");
            return EINVAL;
        }
        return NO_ERROR;
    }

    do {
        NEXT_ARGP_RET();
        if (is_keyword(**argv, "priority"))
            return NO_ERROR;

        bool ref_is_group_ref = false;
        if (indirect_table) {
            if (is_keyword(**argv, "group")) {
                ref_is_group_ref = true;
                NEXT_ARGP_RET();
            }
        } else {
            if (is_keyword(**argv, "counter")) {
                psabpf_direct_counter_context_t dc;
                psabpf_counter_entry_t counter;

                psabpf_direct_counter_ctx_init(&dc);
                psabpf_counter_entry_init(&counter);

                int ret = parse_direct_counter_entry(argc, argv, ctx, entry, &dc, &counter);
                psabpf_counter_entry_free(&counter);
                psabpf_direct_counter_ctx_free(&dc);
                if (ret != NO_ERROR)
                    return ret;

                continue;
            } else if (is_keyword(**argv, "meter")) {
                psabpf_direct_meter_context_t dm;
                psabpf_meter_entry_t meter;

                psabpf_direct_meter_ctx_init(&dm);
                psabpf_meter_entry_init(&meter);

                int ret = parse_direct_meter_entry(argc, argv, ctx, entry, &dm, &meter);
                psabpf_meter_entry_free(&meter);
                psabpf_direct_meter_ctx_free(&dm);
                if (ret != NO_ERROR)
                    return ret;

                continue;
            }
        }

        psabpf_action_param_t param;
        int error_code = translate_data_to_bytes(**argv, &param, CTX_ACTION_DATA);
        if (error_code != NO_ERROR) {
            psabpf_action_param_free(&param);
            return error_code;
        }
        if (ref_is_group_ref)
            psabpf_action_param_mark_group_reference(&param);
        error_code = psabpf_action_param(action, &param);
        if (error_code != NO_ERROR)
            return error_code;
    } while ((*argc) > 1);
    NEXT_ARGP();

    return NO_ERROR;
}

static int parse_entry_priority(int *argc, char ***argv, psabpf_table_entry_t *entry)
{
    if (!is_keyword(**argv, "priority"))
        return NO_ERROR;
    NEXT_ARGP_RET();

    char *ptr;
    psabpf_table_entry_priority(entry, strtoul(**argv, &ptr, 0));
    if (*ptr) {
        fprintf(stderr, "%s: unable to parse priority\n", **argv);
        return EINVAL;
    }
    NEXT_ARGP();

    return NO_ERROR;
}

/******************************************************************************
 * JSON functions
 *****************************************************************************/

json_t *create_JSON_match_key(psabpf_match_key_t *mk)
{
    json_t *root = json_object();
    if (root == NULL)
        return NULL;

    char *value_str = convert_bin_data_to_hexstr(psabpf_matchkey_get_data(mk), psabpf_matchkey_get_data_size(mk));
    char *mask_str = convert_bin_data_to_hexstr(psabpf_matchkey_get_mask(mk), psabpf_matchkey_get_mask_size(mk));
    bool failed = false;

    switch (psabpf_matchkey_get_type(mk)) {
        case PSABPF_EXACT:
            json_object_set_new(root, "type", json_string("exact"));
            if (value_str != NULL)
                json_object_set_new(root, "value", json_string(value_str));
            else
                failed = true;
            break;

        case PSABPF_LPM:
            json_object_set_new(root, "type", json_string("lpm"));
            if (value_str != NULL)
                json_object_set_new(root, "value", json_string(value_str));
            else
                failed = true;
            json_object_set_new(root, "prefix_len", json_integer(psabpf_matchkey_get_prefix(mk)));
            break;

        case PSABPF_TERNARY:
            json_object_set_new(root, "type", json_string("ternary"));
            if (value_str != NULL && mask_str != NULL) {
                json_object_set_new(root, "value", json_string(value_str));
                json_object_set_new(root, "mask", json_string(mask_str));
            } else
                failed = true;
            break;

        default:
            json_object_set_new(root, "type", json_string("unknown"));
    }

    if (failed) {
        fprintf(stderr, "failed to parse match key\n");
        json_decref(root);
        root = NULL;
    }

    if (value_str != NULL)
        free(value_str);
    if (mask_str != NULL)
        free(mask_str);

    return root;
}

json_t *create_JSON_entry_key(psabpf_table_entry_t *entry)
{
    json_t *keys = json_array();
    if (keys == NULL)
        return NULL;

    psabpf_match_key_t *mk = NULL;
    while ((mk = psabpf_table_entry_get_next_matchkey(entry)) != NULL) {
        json_t *key_entry = create_JSON_match_key(mk);
        if (key_entry == NULL)
            return NULL;
        json_array_append_new(keys, key_entry);
        psabpf_matchkey_free(mk);
    }

    return keys;
}

json_t *create_JSON_entry_action_params(psabpf_table_entry_ctx_t *ctx, psabpf_table_entry_t *entry)
{
    json_t *param_root = json_array();
    if (param_root == NULL)
        return NULL;

    psabpf_action_param_t *ap = NULL;
    while ((ap = psabpf_action_param_get_next(entry)) != NULL) {
        json_t *param_entry = json_object();
        if (param_entry == NULL) {
            json_decref(param_root);
            return NULL;
        }
        char *data = convert_bin_data_to_hexstr(psabpf_action_param_get_data(ap),
                                                psabpf_action_param_get_data_len(ap));
        if (data == NULL) {
            json_decref(param_root);
            return NULL;
        }
        const char *name = psabpf_action_param_get_name(ctx, entry, ap);

        if (name != NULL)
            json_object_set_new(param_entry, "name", json_string(name));
        json_object_set_new(param_entry, "value", json_string(data));
        json_array_append(param_root, param_entry);

        free(data);
        psabpf_action_param_free(ap);
    }

    return param_root;
}

json_t *create_JSON_entry_action(psabpf_table_entry_ctx_t *ctx, psabpf_table_entry_t *entry)
{
    json_t *action_root = json_object();
    if (action_root == NULL)
        return NULL;

    uint32_t action_id = psabpf_action_get_id(entry);
    json_object_set_new(action_root, "id", json_integer(action_id));
    const char *action_name = psabpf_action_get_name(ctx, action_id);
    if (action_name != NULL)
        json_object_set_new(action_root, "name", json_string(action_name));

    json_t *action_params = create_JSON_entry_action_params(ctx, entry);
    if (action_params == NULL) {
        json_decref(action_root);
        return NULL;
    }
    json_object_set_new(action_root, "parameters", action_params);

    return action_root;
}

json_t *create_JSON_entry_direct_counter(psabpf_table_entry_ctx_t *ctx, psabpf_table_entry_t *entry)
{
    json_t *counters_root = json_object();
    if (counters_root == NULL)
        return NULL;

    psabpf_direct_counter_context_t *dc_ctx;
    while ((dc_ctx = psabpf_direct_counter_get_next_ctx(ctx, entry)) != NULL) {
        psabpf_counter_entry_t counter;
        int ret = psabpf_direct_counter_get_value(dc_ctx, entry, &counter);
        psabpf_counter_type_t type = psabpf_direct_counter_get_type(dc_ctx);
        const char *name = psabpf_direct_counter_get_name(dc_ctx);

        psabpf_direct_counter_ctx_free(dc_ctx);
        if (ret != NO_ERROR || name == NULL) {
            json_decref(counters_root);
            psabpf_counter_entry_free(&counter);
            return NULL;
        }

        json_t *counter_entry = json_object();
        if (counter_entry == NULL) {
            json_decref(counters_root);
            psabpf_counter_entry_free(&counter);
            return NULL;
        }

        ret = build_json_counter_value(counter_entry, &counter, type);
        psabpf_counter_entry_free(&counter);
        json_object_set_new(counters_root, name, counter_entry);
        if (ret != NO_ERROR) {
            json_decref(counters_root);
            return NULL;
        }
    }

    return counters_root;
}

json_t *create_JSON_entry(psabpf_table_entry_ctx_t *ctx, psabpf_table_entry_t *entry)
{
    json_t *entry_root = json_object();
    if (entry_root == NULL)
        return NULL;

    json_t *key = create_JSON_entry_key(entry);
    if (key == NULL) {
        json_decref(entry_root);
        return NULL;
    }
    json_object_set_new(entry_root, "key", key);

    if (psabpf_table_entry_ctx_has_priority(ctx))
        json_object_set_new(entry_root,
                            "priority",
                            json_integer(psabpf_table_entry_get_priority(entry)));

    if (psabpf_table_entry_ctx_is_indirect(ctx)) {
        /* TODO: references */
    } else {
        json_t *action = create_JSON_entry_action(ctx, entry);
        if (action == NULL) {
            json_decref(entry_root);
            return NULL;
        }
        json_object_set_new(entry_root, "action", action);

        json_t *counters = create_JSON_entry_direct_counter(ctx, entry);
        if (counters == NULL) {
            json_decref(entry_root);
            return NULL;
        }
        json_object_set_new(entry_root, "DirectCounter", counters);
    }

    return entry_root;
}

int print_json_table_entry(psabpf_table_entry_ctx_t *ctx, psabpf_table_entry_t *entry, const char *table_name)
{
    int ret = EINVAL;
    json_t *root = json_object();
    json_t *instance_name = json_object();
    json_t *entries = json_array();

    if (root == NULL || instance_name == NULL || entries == NULL) {
        fprintf(stderr, "failed to prepare JSON\n");
        ret = ENOMEM;
        goto clean_up;
    }

    if (json_object_set(root, table_name, instance_name)) {
        fprintf(stderr, "failed to add JSON key %s\n", table_name);
        goto clean_up;
    }
    json_object_set(instance_name, "entries", entries);

    json_t *parsed_entry = create_JSON_entry(ctx, entry);
    if (parsed_entry == NULL) {
        fprintf(stderr, "failed to create table JSON entry\n");
        goto clean_up;
    }
    json_array_append_new(entries, parsed_entry);

    json_dumpf(root, stdout, JSON_INDENT(4) | JSON_ENSURE_ASCII);
    ret = NO_ERROR;

clean_up:
    json_decref(instance_name);
    json_decref(entries);
    json_decref(root);

    return ret;
}

/******************************************************************************
 * Command line table functions
 *****************************************************************************/

enum table_write_type_t {
    TABLE_ADD_NEW_ENTRY,
    TABLE_UPDATE_EXISTING_ENTRY,
    TABLE_SET_DEFAULT_ENTRY
};

int do_table_write(int argc, char **argv, enum table_write_type_t write_type)
{
    psabpf_table_entry_t entry;
    psabpf_table_entry_ctx_t ctx;
    psabpf_action_t action;
    psabpf_context_t psabpf_ctx;
    int error_code = EPERM;
    bool table_is_indirect = false;

    psabpf_context_init(&psabpf_ctx);
    psabpf_table_entry_ctx_init(&ctx);
    psabpf_table_entry_init(&entry);
    psabpf_action_init(&action);

    /* 0. Get the pipeline id */
    if (parse_pipeline_id(&argc, &argv, &psabpf_ctx) != NO_ERROR)
        goto clean_up;

    /* no NEXT_ARG before in version from this file, so this check must be preserved */
    if (argc < 1) {
        fprintf(stderr, "too few parameters\n");
        goto clean_up;
    }

    /* 1. Get table */
    if (parse_dst_table(&argc, &argv, &psabpf_ctx, &ctx, NULL, false) != NO_ERROR)
        goto clean_up;

    /* 2. Get action */
    bool can_ba_last_arg = write_type == TABLE_SET_DEFAULT_ENTRY ? true : false;
    if (parse_table_action(&argc, &argv, &ctx, &action, &table_is_indirect, can_ba_last_arg) != NO_ERROR)
        goto clean_up;

    /* 3. Get key - default entry has no key */
    if (write_type != TABLE_SET_DEFAULT_ENTRY) {
        if (parse_table_key(&argc, &argv, &entry) != NO_ERROR)
            goto clean_up;
    }

    /* 4. Get action parameters */
    if (parse_action_data(&argc, &argv, &ctx, &entry, &action, table_is_indirect) != NO_ERROR)
        goto clean_up;

    /* 5. Get entry priority - not applicable to default entry */
    if (write_type != TABLE_SET_DEFAULT_ENTRY) {
        if (parse_entry_priority(&argc, &argv, &entry) != NO_ERROR)
            goto clean_up;
    }

    if (argc > 0) {
        fprintf(stderr, "%s: unused argument\n", *argv);
        goto clean_up;
    }

    psabpf_table_entry_action(&entry, &action);

    if (write_type == TABLE_ADD_NEW_ENTRY)
        error_code = psabpf_table_entry_add(&ctx, &entry);
    else if (write_type == TABLE_UPDATE_EXISTING_ENTRY)
        error_code = psabpf_table_entry_update(&ctx, &entry);
    else if (write_type == TABLE_SET_DEFAULT_ENTRY)
        error_code = psabpf_table_entry_set_default_entry(&ctx, &entry);

clean_up:
    psabpf_action_free(&action);
    psabpf_table_entry_free(&entry);
    psabpf_table_entry_ctx_free(&ctx);
    psabpf_context_free(&psabpf_ctx);

    return error_code;
}

int do_table_add(int argc, char **argv)
{
    return do_table_write(argc, argv, TABLE_ADD_NEW_ENTRY);
}

int do_table_update(int argc, char **argv)
{
    return do_table_write(argc, argv, TABLE_UPDATE_EXISTING_ENTRY);
}

int do_table_delete(int argc, char **argv)
{
    psabpf_table_entry_t entry;
    psabpf_table_entry_ctx_t ctx;
    psabpf_context_t psabpf_ctx;
    int error_code = EPERM;

    psabpf_context_init(&psabpf_ctx);
    psabpf_table_entry_ctx_init(&ctx);
    psabpf_table_entry_init(&entry);

    /* 0. Get the pipeline id */
    if (parse_pipeline_id(&argc, &argv, &psabpf_ctx) != NO_ERROR)
        goto clean_up;

    /* no NEXT_ARG before in version from this file, so this check must be preserved */
    if (argc < 1) {
        fprintf(stderr, "too few parameters\n");
        goto clean_up;
    }

    /* 1. Get table */
    if (parse_dst_table(&argc, &argv, &psabpf_ctx, &ctx, NULL, true) != NO_ERROR)
        goto clean_up;

    /* 2. Get key */
    if (parse_table_key(&argc, &argv, &entry) != NO_ERROR)
        goto clean_up;

    if (argc > 0) {
        fprintf(stderr, "%s: unused argument\n", *argv);
        goto clean_up;
    }

    error_code = psabpf_table_entry_del(&ctx, &entry);

clean_up:
    psabpf_table_entry_free(&entry);
    psabpf_table_entry_ctx_free(&ctx);
    psabpf_context_free(&psabpf_ctx);

    return error_code;
}

int do_table_default(int argc, char **argv)
{
    if (is_keyword(*argv, "set")) {
        NEXT_ARG();
        return do_table_write(argc, argv, TABLE_SET_DEFAULT_ENTRY);
    } else {
        if (*argv != NULL)
            fprintf(stderr, "%s: unknown keyword\n", *argv);
        return do_table_help(argc, argv);
    }
}

int do_table_get(int argc, char **argv)
{
    psabpf_table_entry_t entry;
    psabpf_table_entry_ctx_t ctx;
    psabpf_context_t psabpf_ctx;
    int error_code = EPERM;
    const char *table_name = NULL;

    psabpf_context_init(&psabpf_ctx);
    psabpf_table_entry_ctx_init(&ctx);
    psabpf_table_entry_init(&entry);

    /* 0. Get the pipeline id */
    if (parse_pipeline_id(&argc, &argv, &psabpf_ctx) != NO_ERROR)
        goto clean_up;

    /* 1. Get table */
    if (parse_dst_table(&argc, &argv, &psabpf_ctx, &ctx, &table_name, true) != NO_ERROR)
        goto clean_up;

    /* 2. Get key */
    if (parse_table_key(&argc, &argv, &entry) != NO_ERROR)
        goto clean_up;

    if (argc > 0) {
        fprintf(stderr, "%s: unused argument\n", *argv);
        goto clean_up;
    }

    error_code = psabpf_table_entry_get(&ctx, &entry);
    if (error_code != NO_ERROR)
        goto clean_up;

    error_code = print_json_table_entry(&ctx, &entry, table_name);

clean_up:
    psabpf_table_entry_free(&entry);
    psabpf_table_entry_ctx_free(&ctx);
    psabpf_context_free(&psabpf_ctx);

    return error_code;
}

int do_table_help(int argc, char **argv)
{
    (void) argc; (void) argv;

    fprintf(stderr,
            "Usage: %1$s table add pipe ID TABLE ACTION key MATCH_KEY [data ACTION_PARAMS] [priority PRIORITY]\n"
            "       %1$s table add pipe ID TABLE ref key MATCH_KEY data ACTION_REFS [priority PRIORITY]\n"
            "       %1$s table update pipe ID TABLE ACTION key MATCH_KEY [data ACTION_PARAMS] [priority PRIORITY]\n"
            "       %1$s table delete pipe ID TABLE [key MATCH_KEY]\n"
            "       %1$s table default set pipe ID TABLE ACTION [data ACTION_PARAMS]\n"
            /* Support for this one might be preserved, but makes no sense, because indirect tables
             * has no default entry. In other words we do not forbid this syntax explicitly.
             * "       %1$s table default pipe ID TABLE ref data ACTION_REFS\n" */
            "       %1$s table get pipe ID TABLE [key MATCH_KEY]\n"
            "Unimplemented commands:\n"
            "       %1$s table default get pipe ID TABLE\n"
            "\n"
            "       TABLE := { id TABLE_ID | name FILE | TABLE_FILE }\n"
            "       ACTION := { id ACTION_ID | ACTION_NAME }\n"
            "       ACTION_REFS := { MEMBER_REF | group GROUP_REF } \n"
            "       MATCH_KEY := { EXACT_KEY | LPM_KEY | RANGE_KEY | TERNARY_KEY | none }\n"
            "       EXACT_KEY := { DATA }\n"
            "       LPM_KEY := { DATA/PREFIX_LEN }\n"
            /* note: simple_switch_CLI uses '->' for range match, but this is
             *   harder to write in a CLI (needs an escape sequence) */
            "       RANGE_KEY := { DATA_MIN..DATA_MAX }\n"
            /* note: by default '&&&' is used but it also will require
             *   an escape sequence in a CLI, so lets use '^' instead */
            "       TERNARY_KEY := { DATA^MASK }\n"
            "       ACTION_PARAMS := { DATA | counter COUNTER_NAME COUNTER_VALUE | meter METER_NAME METER_VALUE }\n"
            "       COUNTER_VALUE := { BYTES | PACKETS | BYTES:PACKETS }\n"
            "       METER_VALUE := { PIR:PBS CIR:CBS }\n"
            "",
            program_name);
    return 0;
}
