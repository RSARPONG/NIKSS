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

#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <dirent.h>
#include <errno.h>
#include <ftw.h>
#include <linux/if_link.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include <nikss/nikss_pipeline.h>

#include "bpf_defs.h"
#include "btf.h"
#include "common.h"

static char *program_pin_name(struct bpf_program *prog)
{
    char *name = NULL;
    char *p = NULL;

    name = p = strdup(bpf_program__section_name(prog));
    while ((p = strchr(p, '/'))) {
        *p = '_';
    }

    return name;
}

static int do_initialize_maps(int prog_fd)
{
    char in[128];
    char out[128];
    /* error in errno (sys call) */
    return bpf_prog_test_run(prog_fd, 1, &in[0], 128,
                             out, NULL, NULL, NULL);
}

static int open_prog_by_name(nikss_context_t *ctx, const char *prog)
{
    char pinned_file[256];
    build_ebpf_prog_filename(pinned_file, sizeof(pinned_file), ctx, prog);

    return bpf_obj_get(pinned_file);  // error in errno
}

static int tc_create_hook(int ifindex, const char *interface)
{
    DECLARE_LIBBPF_OPTS(bpf_tc_hook, hook,
                        .ifindex = ifindex,
                        .attach_point = BPF_TC_INGRESS | BPF_TC_EGRESS);

    int ret = NO_ERROR;
    if (bpf_tc_hook_create(&hook) != 0) {
        ret = errno;
        fprintf(stderr, "failed to create TC hook for interface %s: %s\n", interface, strerror(ret));
    }

    return ret;
}

static int tc_attach_prog(nikss_context_t *ctx, const char *prog, int ifindex, enum bpf_tc_attach_point hook_point, const char *interface)
{
    int ret = NO_ERROR;
    int fd = open_prog_by_name(ctx, prog);
    if (fd < 0) {
        ret = errno;
        if (ret == ENOENT && hook_point == BPF_TC_EGRESS) {
            fprintf(stderr, "skipping empty egress program...\n");
            return NO_ERROR;
        }

        fprintf(stderr, "failed to open program %s: %s\n", prog, strerror(ret));
        return ret;
    }

    DECLARE_LIBBPF_OPTS(bpf_tc_hook, hook,
                        .ifindex = ifindex,
                        .attach_point = hook_point);
    DECLARE_LIBBPF_OPTS(bpf_tc_opts, opts,
                        .prog_fd = fd);

    if (bpf_tc_attach(&hook, &opts) != 0) {
        ret = errno;
        fprintf(stderr, "failed to attach bpf program to interface %s: %s\n", interface, strerror(ret));
        goto clean_up;
    }

clean_up:
    close(fd);

    return ret;
}

static int tc_create_hook_and_attach_progs(nikss_context_t *ctx, int ifindex, const char *interface)
{
    int ret = tc_create_hook(ifindex, interface);
    if (ret != NO_ERROR) {
        return ret;
    }

    ret = tc_attach_prog(ctx, TC_INGRESS_PROG, ifindex, BPF_TC_INGRESS, interface);
    if (ret != NO_ERROR) {
        return ret;
    }

    ret = tc_attach_prog(ctx, TC_EGRESS_PROG, ifindex, BPF_TC_EGRESS, interface);
    if (ret != NO_ERROR) {
        return ret;
    }

    return NO_ERROR;
}

static int xdp_attach_prog_to_port(int *fd, nikss_context_t *ctx, int ifindex, const char *prog)
{
    __u32 flags = 0;
    int ret = 0;

    *fd = open_prog_by_name(ctx, prog);
    if (*fd < 0) {
        ret = errno;  // from sys_call
        fprintf(stderr, "failed to open program %s: %s\n", prog, strerror(ret));
        return ret;
    }

    /* TODO: add support for hardware offload mode (XDP_FLAGS_HW_MODE) */

    flags = XDP_FLAGS_DRV_MODE;
    ret = bpf_set_link_xdp_fd(ifindex, *fd, flags);
    if (ret != -EOPNOTSUPP) {
        if (ret < 0) {
            fprintf(stderr, "failed to attach XDP program in driver mode: %s\n", strerror(-ret));
            close_object_fd(fd);
            return -ret;
        }
        return NO_ERROR;
    }

    fprintf(stderr, "XDP native mode not supported by driver, retrying with generic SKB mode\n");
    flags = XDP_FLAGS_SKB_MODE;
    ret = bpf_set_link_xdp_fd(ifindex, *fd, flags);
    if (ret < 0) {
        fprintf(stderr, "failed to attach XDP program in SKB mode: %s\n", strerror(-ret));
        close_object_fd(fd);
        return -ret;
    }

    return NO_ERROR;
}

static int update_prog_devmap(nikss_bpf_map_descriptor_t *devmap, int ifindex, const char *intf, int egress_prog_fd)
{
    struct bpf_devmap_val devmap_val;

    devmap_val.ifindex = ifindex;
    devmap_val.bpf_prog.fd = -1;

    /* install egress program only if it's found */
    if (egress_prog_fd >= 0) {
        devmap_val.bpf_prog.fd = egress_prog_fd;
    }
    if (ifindex > (int) devmap->max_entries) {
        fprintf(stderr,
                "Warning: the index(=%d) of the interface %s is higher than the DEVMAP size (=%u)\n"
                "Applying modulo ... \n", ifindex, intf, devmap->max_entries);
    }
    int index = ifindex % ((int) devmap->max_entries);
    int ret = bpf_map_update_elem(devmap->fd, &index, &devmap_val, 0);
    if (ret) {
        ret = errno;
        fprintf(stderr, "failed to update devmap: %s\n", strerror(ret));
        return ret;
    }

    return NO_ERROR;
}

static int xdp_port_add(nikss_context_t *ctx, const char *intf, int ifindex)
{
    int ret = NO_ERROR;
    int ig_prog_fd = 0;
    int eg_prog_fd = 0;

    /* TODO: Should we attach ingress pipeline at the end of whole procedure?
     *  For short time packets will be served only in ingress but not in egress pipeline. */
    ret = xdp_attach_prog_to_port(&ig_prog_fd, ctx, ifindex, XDP_INGRESS_PROG);
    if (ret != NO_ERROR) {
        return ret;
    }
    close_object_fd(&ig_prog_fd);

    /* may not exist, ignore errors */
    eg_prog_fd = open_prog_by_name(ctx, XDP_EGRESS_PROG);

    nikss_bpf_map_descriptor_t devmap;
    ret = open_bpf_map(ctx, XDP_DEVMAP, NULL, &devmap);
    if (ret != NO_ERROR) {
        fprintf(stderr, "failed to open DEVMAP: %s\n", strerror(ret));
        close_object_fd(&eg_prog_fd);
        return ret;
    }

    ret = update_prog_devmap(&devmap, ifindex, intf, eg_prog_fd);
    close_object_fd(&eg_prog_fd);
    close_object_fd(&devmap.fd);
    if (ret != NO_ERROR) {
        return ret;
    }

    eg_prog_fd = open_prog_by_name(ctx, XDP_EGRESS_PROG_OPTIMIZED);
    if (eg_prog_fd >= 0) {
        nikss_bpf_map_descriptor_t jmpmap;
        ret = open_bpf_map(ctx, XDP_JUMP_TBL, NULL, &jmpmap);
        if (ret != NO_ERROR) {
            fprintf(stderr, "failed to open map %s: %s\n", XDP_JUMP_TBL, strerror(errno));
            close_object_fd(&eg_prog_fd);
            return ENOENT;
        }

        int index = 0;
        ret = bpf_map_update_elem(jmpmap.fd, &index, &eg_prog_fd, 0);
        int errno_val = errno;
        close_object_fd(&eg_prog_fd);
        close_object_fd(&jmpmap.fd);
        if (ret) {
            fprintf(stderr, "failed to update map %s: %s\n", XDP_JUMP_TBL, strerror(errno_val));
            return errno_val;
        }
    }

    ret = tc_create_hook_and_attach_progs(ctx, ifindex, intf);
    if (ret != NO_ERROR) {
        return ret;
    }

    return NO_ERROR;
}

static int tc_port_add(nikss_context_t *ctx, const char *interface, int ifindex)
{
    int xdp_helper_fd = -1;

    int ret = xdp_attach_prog_to_port(&xdp_helper_fd, ctx, ifindex, XDP_HELPER_PROG);
    if (ret != NO_ERROR) {
        return ret;
    }
    close_object_fd(&xdp_helper_fd);

    ret = tc_create_hook_and_attach_progs(ctx, ifindex, interface);
    if (ret != NO_ERROR) {
        return ret;
    }

    return NO_ERROR;
}

bool nikss_pipeline_exists(nikss_context_t *ctx)
{
    char mounted_path[256];
    build_ebpf_pipeline_path(mounted_path, sizeof(mounted_path), ctx);

    return access(mounted_path, F_OK) == 0;
}

static int extract_tuple_id_from_tuple(const char *tuple_name, uint32_t *tuple_id)
{
    char *elem = NULL;
    elem = strrchr(tuple_name, '_');
    elem++;
    if (tuple_id != NULL) {
        char *end = NULL;
        *tuple_id = (uint32_t)strtol(elem, &end, 10);
        if (elem == end) {
            return ENODATA;
        }
    } else {
        return EINVAL;
    }
    return NO_ERROR;
}

static int join_tuple_to_map_if_tuple(nikss_context_t *ctx, const char *tuple_name)
{
    // We assume that each tuple has "_tuple_" suffix
    // This name also is reserved in a p4c-ebpf-psa compiler
    const char *suffix = "_tuple_";
    const char *ternary_tbl_name_lst_char_ptr = strstr(tuple_name, suffix);

    if (ternary_tbl_name_lst_char_ptr) {
        char tuples_map_name[268];
        int ternary_map_name_length = (int)(ternary_tbl_name_lst_char_ptr - tuple_name);
        snprintf(tuples_map_name, sizeof(tuples_map_name), "%.*s_tuples_map", ternary_map_name_length, tuple_name);

        nikss_bpf_map_descriptor_t tuple_map;
        int ret = open_bpf_map(ctx, tuples_map_name, NULL, &tuple_map);
        if (ret != NO_ERROR) {
            fprintf(stderr, "couldn't open map %s: %s\n", tuples_map_name, strerror(ret));
            return ret;
        }

        // Take tuple_id from a tuple map name
        uint32_t tuple_id = 0;
        ret = extract_tuple_id_from_tuple(tuple_name, &tuple_id);
        if (ret != NO_ERROR) {
            fprintf(stderr, "cannot extract tuple_id from tuple name %s: %s", tuple_name, strerror(ret));
            return ENODATA;
        }

        nikss_bpf_map_descriptor_t tuple;
        ret = open_bpf_map(ctx, tuple_name, NULL, &tuple);
        if (ret != NO_ERROR) {
            fprintf(stderr, "couldn't open map %s: %s\n", tuple_name, strerror(ret));
            return ret;
        }

        ret = bpf_map_update_elem(tuple_map.fd, &tuple_id, &tuple.fd, 0);
        if (ret != NO_ERROR) {
            fprintf(stderr, "failed to add tuple %u: %s\n", tuple_id, strerror(ret));
        }

        tuple_id++;
    }

    return NO_ERROR;
}

int nikss_pipeline_load(nikss_context_t *ctx, const char *file)
{
    struct bpf_object *obj = NULL;
    int ret = 0;
    int fd = -1;
    char pinned_file[256];
    struct bpf_program *pos = NULL;

    ret = bpf_prog_load(file, BPF_PROG_TYPE_UNSPEC, &obj, &fd);
    /* Do not close fd obtained from above call, it is maintained by obj */
    if (ret < 0 || obj == NULL) {
        ret = errno;
        fprintf(stderr, "cannot load the BPF program: %s\n", strerror(ret));
        return ret;
    }

    bpf_object__for_each_program(pos, obj) {
        const char *sec_name = bpf_program__section_name(pos);
        char *pin_name = program_pin_name(pos);

        build_ebpf_prog_filename(pinned_file, sizeof(pinned_file),
                                 ctx, pin_name);
        free(pin_name);

        ret = bpf_program__pin(pos, pinned_file);
        if (ret < 0) {
            fprintf(stderr, "failed to pin %s at %s: %s\n",
                    sec_name, pinned_file, strerror(-ret));
            goto err_close_obj;
        }
    }

    struct bpf_map *map = NULL;
    bpf_object__for_each_map(map, obj) {
        if (bpf_map__is_pinned(map)) {
            ret = bpf_map__unpin(map, NULL);
            if (ret) {
                fprintf(stderr, "failed to remove old map pin file: %s\n", strerror(-ret));
                goto err_close_obj;
            }
        }

        const char *map_name = bpf_map__name(map);

        /* Pinned file name cannot contain a dot */
        if (strstr(map_name, ".") != NULL) {
            continue;
        }

        build_ebpf_map_filename(pinned_file, sizeof(pinned_file), ctx, map_name);
        ret = bpf_map__set_pin_path(map, pinned_file);
        if (ret) {
            fprintf(stderr, "failed to pin map at %s: %s\n", pinned_file, strerror(-ret));
            goto err_close_obj;
        }

        ret = bpf_map__pin(map, pinned_file);
        if (ret) {
            fprintf(stderr, "failed to pin map at %s: %s\n", pinned_file, strerror(-ret));
            goto err_close_obj;
        }

        ret = join_tuple_to_map_if_tuple(ctx, map_name);
        if (ret) {
            fprintf(stderr, "failed to add tuple (%s) to tuples map\n", map_name);
            goto err_close_obj;
        }
    }

    bpf_object__for_each_program(pos, obj) {
        const char *sec_name = bpf_program__section_name(pos);
        fd = bpf_program__fd(pos);
        if (!strcmp(sec_name, TC_INIT_PROG) || !strcmp(sec_name, XDP_INIT_PROG)) {
            ret = do_initialize_maps(fd);
            if (ret) {
                ret = -errno;
                fprintf(stderr, "failed to initialize maps: %s\n", strerror(errno));
                goto err_close_obj;
            }
        }
    }

err_close_obj:
    bpf_object__close(obj);

    /* ret is negative value from returned libbpf, but we should return positive ones */
    return -ret;
}

static int remove_file(const char *fpath, const struct stat *sb, int tflag, struct FTW *ftwbuf)
{
    (void) sb; (void) tflag; (void) ftwbuf;

    /* Ignore any error and continue */
    remove(fpath);
    return 0;
}

static int remove_pipeline_directory(nikss_context_t *ctx)
{
    char pipeline_path[256];
    build_ebpf_pipeline_path(pipeline_path, sizeof(pipeline_path), ctx);

    /* 16 - Maximum number of file descriptors used by nftw(). In case lack of
     *      available file descriptors it can be reduced at the cost of performance.
     * FTW_DEPTH - Call callback for the directory itself after handling the
     *             contents of the directory and its subdirectories.
     * FTW_MOUNT - Stay within the same filesystem (i.e., do not cross mount points).
     * FTW_PHYS  - Do  not  follow  symbolic  links. */
    if (nftw(pipeline_path, remove_file, 16, FTW_DEPTH | FTW_MOUNT | FTW_PHYS) != 0) {
        int err = errno;
        fprintf(stderr, "failed to remove pipeline directory: %s\n", strerror(err));
        return err;
    }

    return NO_ERROR;
}

int nikss_pipeline_unload(nikss_context_t *ctx)
{
    /* TODO: Should we scan all interfaces to detect if it uses current pipeline programs and detach it? */

    return remove_pipeline_directory(ctx);
}

int nikss_pipeline_add_port(nikss_context_t *ctx, const char *interface, int *port_id)
{
    char pinned_file[256];
    bool isXDP = false;

    /* Determine firstly if we have TC-based or XDP-based pipeline.
     * We can do this by just checking if XDP helper exists under a mount path. */
    build_ebpf_prog_filename(pinned_file, sizeof(pinned_file), ctx, XDP_HELPER_PROG);
    isXDP = access(pinned_file, F_OK) != 0;

    int ifindex = (int) if_nametoindex(interface);
    if (!ifindex) {
        fprintf(stderr, "no such interface: %s\n", interface);
        return ENODEV;
    }

    if (port_id != NULL) {
        *port_id = ifindex;
    }

    return isXDP ? xdp_port_add(ctx, interface, ifindex) : tc_port_add(ctx, interface, ifindex);
}

int nikss_pipeline_del_port(nikss_context_t *ctx, const char *interface)
{
    (void) ctx;
    __u32 flags = 0;
    int ifindex = 0;

    ifindex = (int) if_nametoindex(interface);
    if (!ifindex) {
        fprintf(stderr, "no such interface: %s\n", interface);
        return ENODEV;
    }

    int ret = bpf_set_link_xdp_fd(ifindex, -1, flags);
    if (ret) {
        fprintf(stderr, "failed to detach XDP program: %s\n", strerror(-ret));
        return -ret;
    }

    DECLARE_LIBBPF_OPTS(bpf_tc_hook, hook,
                        .ifindex = ifindex,
                        .attach_point = BPF_TC_INGRESS | BPF_TC_EGRESS);
    if (bpf_tc_hook_destroy(&hook) != 0) {
        ret = errno;
        /* Ignore error when qdisc does not exist, e.g. for XDP dummy program */
        if (ret != ENOENT) {
            fprintf(stderr, "failed to detach TC program from %s: %s\n", interface, strerror(ret));
            return ret;
        }
    }

    return NO_ERROR;
}

int nikss_port_list_init(nikss_port_list_t *list, nikss_context_t *ctx)
{
    int ret = NO_ERROR;
    if (list == NULL || ctx == NULL) {
        return EINVAL;
    }

    memset(list, 0, sizeof(nikss_port_list_t));

    list->iface_list = if_nameindex();
    if (list->iface_list == NULL) {
        return errno;
    }

    int fd = open_prog_by_name(ctx, XDP_HELPER_PROG);
    if (fd < 0) {
        /* XDP helper not found, try XDP ingress program */
        fd = open_prog_by_name(ctx, XDP_INGRESS_PROG);
    }

    if (fd < 0) {
        ret = errno;
        fprintf(stderr, "failed to open pipeline program: %s\n", strerror(ret));
        return ret;
    }

    struct bpf_prog_info prog_info = {};
    unsigned len = sizeof(struct bpf_prog_info);
    if (bpf_obj_get_info_by_fd(fd, &prog_info, &len) != 0) {
        ret = errno;
        fprintf(stderr, "failed to get BPF program info: %s\n", strerror(ret));
        goto free_program;
    }

    list->xdp_prog_id = prog_info.id;

free_program:
    close(fd);
    return ret;
}

void nikss_port_list_free(nikss_port_list_t *list)
{
    if (list == NULL) {
        return;
    }

    if (list->iface_list != NULL) {
        if_freenameindex(list->iface_list);
    }

    list->iface_list = NULL;
    list->current_iface = NULL;
}

nikss_port_spec_t * nikss_port_list_get_next_port(nikss_port_list_t *list)
{
    if (list == NULL) {
        return NULL;
    }
    if (list->iface_list == NULL) {
        return NULL;
    }

    bool iface_found = false;

    while (!iface_found) {
        if (list->current_iface == NULL) {
            list->current_iface = list->iface_list;
        } else {
            list->current_iface = ((struct if_nameindex *) list->current_iface) + 1;
        }

        list->current_port.id = ((struct if_nameindex *) list->current_iface)->if_index;
        list->current_port.name = ((struct if_nameindex *) list->current_iface)->if_name;

        if (list->current_port.id == 0 || list->current_port.name == NULL) {
            /* End of the list */
            list->current_iface = NULL;
            return NULL;
        }

        uint32_t prog_id = 0;
        int ret = bpf_get_link_xdp_id((int) list->current_port.id, &prog_id, 0);
        if (ret != 0 || prog_id == 0) {
            continue;
        }

        if (prog_id == list->xdp_prog_id) {
            iface_found = true;
            break;
        }
    }

    if (!iface_found) {
        return NULL;
    }
    return &list->current_port;
}

const char * nikss_port_spec_get_name(nikss_port_spec_t *port)
{
    if (port == NULL) {
        return NULL;
    }

    return port->name;
}

unsigned nikss_port_sepc_get_id(nikss_port_spec_t *port)
{
    if (port == NULL) {
        return 0;
    }

    return port->id;
}

void nikss_port_spec_free(nikss_port_spec_t *port)
{
    (void) port;
}

uint64_t nikss_pipeline_get_load_timestamp(nikss_context_t *ctx)
{
    uint64_t load_timestamp = 0;
    int fd = open_prog_by_name(ctx, XDP_HELPER_PROG);
    if (fd < 0) {
        /* XDP helper not found, try XDP ingress program */
        fd = open_prog_by_name(ctx, XDP_INGRESS_PROG);
    }

    if (fd < 0) {
        fprintf(stderr, "failed to open pipeline program: %s\n", strerror(errno));
        return 0;
    }

    struct bpf_prog_info prog_info = {};
    unsigned len = sizeof(struct bpf_prog_info);
    if (bpf_obj_get_info_by_fd(fd, &prog_info, &len) != 0) {
        fprintf(stderr, "failed to get BPF program info: %s\n", strerror(errno));
        goto clean_up;
    }
    double load_time = (double) prog_info.load_time / 1e9;

    double uptime = 0;
    /* Add O_CLOEXEC option to not propagate file descriptor to children processes when library used in forking server */
    FILE *uptime_file = fopen("/proc/uptime", "re");
    if (uptime_file != NULL) {
        char buf[BUFSIZ];
        char *b = fgets(buf, BUFSIZ, uptime_file);
        fclose(uptime_file);

        char *end_ptr = NULL;
        if (b == buf) {
            uptime = strtod(buf, &end_ptr);
        } else {
            goto clean_up;
        }
    } else {
        fprintf(stderr, "failed to get uptime: %s\n", strerror(errno));
        goto clean_up;
    }

    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0) {
        fprintf(stderr, "failed to get current time: %s\n", strerror(errno));
        goto clean_up;
    }
    double now = (double) tv.tv_sec + ((double) tv.tv_usec) / 1e6;

    load_timestamp = (uint64_t) (now - uptime + load_time);

clean_up:
    close(fd);
    return load_timestamp;
}

static bool check_if_program_exists(nikss_context_t *ctx, const char *prog)
{
    char pinned_file[256];
    build_ebpf_prog_filename(pinned_file, sizeof(pinned_file), ctx, prog);

    return access(pinned_file, F_OK) == 0;
}

bool nikss_pipeline_is_TC_based(nikss_context_t *ctx)
{
    if (ctx == NULL) {
        return false;
    }
    return check_if_program_exists(ctx, XDP_HELPER_PROG) &&
           !check_if_program_exists(ctx, XDP_INGRESS_PROG) &&
           !check_if_program_exists(ctx, XDP_EGRESS_PROG) &&
           !check_if_program_exists(ctx, XDP_EGRESS_PROG_OPTIMIZED);
}

bool nikss_pipeline_has_egress_program(nikss_context_t *ctx)
{
    if (ctx == NULL) {
        return false;
    }
    return check_if_program_exists(ctx, TC_EGRESS_PROG) ||
           check_if_program_exists(ctx, XDP_EGRESS_PROG) ||
           check_if_program_exists(ctx, XDP_EGRESS_PROG_OPTIMIZED);
}

int nikss_pipeline_objects_list_init(nikss_pipeline_objects_list_t *list, nikss_context_t *ctx)
{
    if (list == NULL || ctx == NULL) {
        return EINVAL;
    }

    memset(list, 0, sizeof(nikss_pipeline_objects_list_t));

    build_ebpf_map_filename(&list->base_objects_path[0], sizeof(list->base_objects_path), ctx, "");
    list->directory = opendir(&list->base_objects_path[0]);
    if (list->directory == NULL) {
        return errno;
    }

    return NO_ERROR;
}

void nikss_pipeline_objects_list_free(nikss_pipeline_objects_list_t *list)
{
    if (list == NULL) {
        return;
    }

    if (list->directory != NULL) {
        closedir(list->directory);
    }
    list->directory = NULL;
}

bool is_valid_object_name(nikss_pipeline_objects_list_t *list, const char *name,
                          const char *allowed_suffixes[], const unsigned no_allowed_suffixes)
{
    const char *reserved_names[] = {
            "clone_session_tbl",
            "clone_session_tbl_inner",
            "multicast_grp_tbl",
            "multicast_grp_tbl_inner",
            "hdr_md_cpumap",
            "xdp2tc_shared_map",
            "xdp2tc_cpumap",
            "tx_port",
            "crc_lookup_tbl",
    };
    const char *reserved_prefixes[] = {
            "ebpf_",
    };
    const char *suffixes[] = {
            "_defaultAction",
            "_prefixes",
            "_tuple",
            "_tuples_map",
            "_groups_inner",
            "_groups",
            "_defaultActionGroup",
            "_actions",
    };
    /* cppcheck-suppress variableScope ; for readability all related variables are defined together */
    const char *ternary_tuple_infix = "_tuple_";
    const unsigned no_reserved_names = sizeof(reserved_names) / sizeof(reserved_names[0]);
    const unsigned no_reserved_prefixes = sizeof(reserved_prefixes) / sizeof(reserved_prefixes[0]);
    const unsigned no_suffixes = sizeof(suffixes) / sizeof(suffixes[0]);

    /* Reserved names are not allowed (exact match) */
    for (unsigned i = 0; i < no_reserved_names; ++i) {
        if (strcmp(name, reserved_names[i]) == 0) {
            return false;
        }
    }

    /* Reserved prefixes are not allowed */
    for (unsigned i = 0; i < no_reserved_prefixes; ++i) {
        if (strncmp(name, reserved_prefixes[i], strlen(reserved_prefixes[i])) == 0) {
            return false;
        }
    }

    /* Check for known suffix */
    bool has_suffix = false;
    for (unsigned i = 0; i < no_suffixes; ++i) {
        if (str_ends_with(name, suffixes[i])) {
            has_suffix = true;
            break;
        }
    }

    /* No suffix no problem. Unless we have a ternary tuple */
    if (!has_suffix) {
        if (strstr(name, ternary_tuple_infix) != NULL) {
            return false;
        }
        return true;
    }

    /* Allow occurrence of some suffixes */
    for (unsigned i = 0; i < no_allowed_suffixes; ++i) {
        if (str_ends_with(name, allowed_suffixes[i])) {
            return true;
        }
    }

    /* Let's check whether there is object which has additional suffix, e.g. ends with "_groups_groups" */
    char path[512];
    for (unsigned i = 0; i < no_suffixes; ++i) {
        snprintf(path, sizeof(path), "%s%s%s", list->base_objects_path, name, suffixes[i]);
        if (access(path, F_OK) == 0) {
            return true;
        }
    }

    return false;
}

nikss_pipeline_object_t * nikss_pipeline_objects_list_get_next_object(nikss_pipeline_objects_list_t *list)
{
    if (list == NULL) {
        return NULL;
    }
    if (list->directory == NULL) {
        return NULL;
    }

    /* Some object has no direct names in the file system, they occur only with suffix(es) */
    const char *allowed_suffixes[] = {
            "_prefixes",
            "_actions",
    };
    const unsigned no_allowed_suffixes = sizeof(allowed_suffixes) / sizeof(allowed_suffixes[0]);

    struct dirent *file = NULL;
    while ((file = readdir(list->directory)) != NULL) {
        if (file->d_type != DT_REG) {
            continue;
        }

        if (!is_valid_object_name(list, file->d_name, allowed_suffixes, no_allowed_suffixes)) {
            continue;
        }

        snprintf(&list->current_object.name[0], sizeof(list->current_object.name), "%s", file->d_name);
        for (unsigned i = 0; i < no_allowed_suffixes; ++i) {
            /* Remove only one suffix */
            if (remove_suffix_from_str(&list->current_object.name[0], allowed_suffixes[i])) {
                break;
            }
        }

        return &list->current_object;
    }

    return NULL;
}

const char * nikss_pipeline_object_get_name(nikss_pipeline_object_t *obj)
{
    if (obj == NULL) {
        return NULL;
    }
    return &obj->name[0];
}

void nikss_pipeline_object_free(nikss_pipeline_object_t *obj)
{
    (void) obj;
}
