/**
 * @file context.c
 * @author Radek Krejci <rkrejci@cesnet.cz>
 * @brief Context implementations
 *
 * Copyright (c) 2015 - 2018 CESNET, z.s.p.o.
 *
 * This source code is licensed under BSD 3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://opensource.org/licenses/BSD-3-Clause
 */

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "context.h"
#include "common.h"

#include "libyang.h"

#define LY_INTERNAL_MODS_COUNT 0 /* TODO 6 when parser available */

#define IETF_YANG_METADATA_PATH "../models/ietf-yang-metadata@2016-08-05.h"
#define YANG_PATH "../models/yang@2017-02-20.h"
#define IETF_INET_TYPES_PATH "../models/ietf-inet-types@2013-07-15.h"
#define IETF_YANG_TYPES_PATH "../models/ietf-yang-types@2013-07-15.h"
#define IETF_DATASTORES "../models/ietf-datastores@2017-08-17.h"
#define IETF_YANG_LIB_PATH "../models/ietf-yang-library@2018-01-17.h"
#define IETF_YANG_LIB_REV "2018-01-17"

#if LY_INTERNAL_MODS_COUNT
#include IETF_YANG_METADATA_PATH
#include YANG_PATH
#include IETF_INET_TYPES_PATH
#include IETF_YANG_TYPES_PATH
#include IETF_DATASTORES
#include IETF_YANG_LIB_PATH

static struct internal_modules_s {
    const char *name;
    const char *revision;
    const char *data;
    uint8_t implemented;
    LYS_INFORMAT format;
} internal_modules[LY_INTERNAL_MODS_COUNT] = {
    {"ietf-yang-metadata", "2016-08-05", (const char*)ietf_yang_metadata_2016_08_05_yin, 0, LYS_IN_YIN},
    {"yang", "2017-02-20", (const char*)yang_2017_02_20_yin, 1, LYS_IN_YIN},
    {"ietf-inet-types", "2013-07-15", (const char*)ietf_inet_types_2013_07_15_yin, 0, LYS_IN_YIN},
    {"ietf-yang-types", "2013-07-15", (const char*)ietf_yang_types_2013_07_15_yin, 0, LYS_IN_YIN},
    /* ietf-datastores and ietf-yang-library must be right here at the end of the list! */
    {"ietf-datastores", "2017-08-17", (const char*)ietf_datastores_2017_08_17_yin, 0, LYS_IN_YIN},
    {"ietf-yang-library", IETF_YANG_LIB_REV, (const char*)ietf_yang_library_2018_01_17_yin, 1, LYS_IN_YIN}
};
#endif

API LY_ERR
ly_ctx_set_searchdir(struct ly_ctx *ctx, const char *search_dir)
{
    char *new_dir = NULL;
    LY_ERR rc = LY_ESYS;

    LY_CHECK_ARG_RET(ctx, ctx, LY_EINVAL);

    if (search_dir) {
        LY_CHECK_ERR_GOTO(access(search_dir, R_OK | X_OK),
                          LOGERR(ctx, LY_ESYS, "Unable to use search directory \"%s\" (%s)", search_dir, strerror(errno)),
                          cleanup);
        new_dir = realpath(search_dir, NULL);
        LY_CHECK_ERR_GOTO(!new_dir,
                          LOGERR(ctx, LY_ESYS, "realpath() call failed for \"%s\" (%s).", search_dir, strerror(errno)),
                          cleanup);
        if (ly_set_add(&ctx->search_paths, new_dir, 0) == -1) {
            free(new_dir);
            return LY_EMEM;
        }

        rc = LY_SUCCESS;
    } else {
        /* consider that no change is not actually an error */
        return LY_SUCCESS;
    }

cleanup:
    free(new_dir);
    return rc;
}

API const char * const *
ly_ctx_get_searchdirs(const struct ly_ctx *ctx)
{
    LY_CHECK_ARG_RET(ctx, ctx, NULL);
    return (const char * const *)ctx->search_paths.objs;
}

API LY_ERR
ly_ctx_unset_searchdirs(struct ly_ctx *ctx, int index)
{
    if (!ctx->search_paths.count) {
        return LY_SUCCESS;
    }

    if (index >= 0) {
        /* remove specific search directory */
        return ly_set_rm_index(&ctx->search_paths, index);
    } else {
        /* remove them all */
        for (; ctx->search_paths.count; ctx->search_paths.count--) {
            free(ctx->search_paths.objs[ctx->search_paths.count - 1]);
        }
        free(ctx->search_paths.objs);
        memset(&ctx->search_paths, 0, sizeof ctx->search_paths);
    }

    return LY_SUCCESS;
}

API LY_ERR
ly_ctx_new(const char *search_dir, int options, struct ly_ctx **new_ctx)
{
    struct ly_ctx *ctx = NULL;
    struct lys_module *module;
    char *search_dir_list;
    char *sep, *dir;
    LY_ERR rc = LY_SUCCESS;

    ctx = calloc(1, sizeof *ctx);
    LY_CHECK_ERR_RET(!ctx, LOGMEM(NULL), LY_EMEM);

    /* dictionary */
    lydict_init(&ctx->dict);

#if 0 /* TODO when plugins implemented */
    /* plugins */
    ly_load_plugins();
#endif

    /* initialize thread-specific key */
    while ((pthread_key_create(&ctx->errlist_key, ly_err_free)) == EAGAIN);

    /* models list */
    ctx->flags = options;
    if (search_dir) {
        search_dir_list = strdup(search_dir);
        LY_CHECK_ERR_GOTO(!search_dir_list, LOGMEM(NULL); rc = LY_EMEM, error);

        for (dir = search_dir_list; (sep = strchr(dir, ':')) != NULL && rc == LY_SUCCESS; dir = sep + 1) {
            *sep = 0;
            rc = ly_ctx_set_searchdir(ctx, dir);
        }
        if (*dir && rc == LY_SUCCESS) {
            rc = ly_ctx_set_searchdir(ctx, dir);
        }
        free(search_dir_list);

        /* If ly_ctx_set_searchdir() failed, the error is already logged. Just exit */
        if (rc != LY_SUCCESS) {
            goto error;
        }
    }
    ctx->module_set_id = 1;

#if 0 /* TODO when parser implemented */
    /* load internal modules */
    for (i = 0; i < (options & LY_CTX_NOYANGLIBRARY) ? LY_INTERNAL_MODS_COUNT - 2 : LY_INTERNAL_MODS_COUNT; i++) {
        module = (struct lys_module *)lys_parse_mem(ctx, internal_modules[i].data, internal_modules[i].format);
        LY_CHECK_GOTO(!module, error);
        module->parsed->implemented = internal_modules[i].implemented;
    }
#endif

    *new_ctx = ctx;
    return rc;

error:
    ly_ctx_destroy(ctx, NULL);
    return rc;
}

API void
ly_ctx_set_disable_searchdirs(struct ly_ctx *ctx)
{
    LY_CHECK_ARG_RET(ctx, ctx,);
    ctx->flags |= LY_CTX_DISABLE_SEARCHDIRS;
}

API void
ly_ctx_unset_disable_searchdirs(struct ly_ctx *ctx)
{
    LY_CHECK_ARG_RET(ctx, ctx,);
    ctx->flags &= ~LY_CTX_DISABLE_SEARCHDIRS;
}

API void
ly_ctx_set_disable_searchdir_cwd(struct ly_ctx *ctx)
{
    LY_CHECK_ARG_RET(ctx, ctx,);
    ctx->flags |= LY_CTX_DISABLE_SEARCHDIR_CWD;
}

API void
ly_ctx_unset_disable_searchdir_cwd(struct ly_ctx *ctx)
{
    LY_CHECK_ARG_RET(ctx, ctx,);
    ctx->flags &= ~LY_CTX_DISABLE_SEARCHDIR_CWD;
}

API void
ly_ctx_set_prefer_searchdirs(struct ly_ctx *ctx)
{
    LY_CHECK_ARG_RET(ctx, ctx,);
    ctx->flags |= LY_CTX_PREFER_SEARCHDIRS;
}

API void
ly_ctx_unset_prefer_searchdirs(struct ly_ctx *ctx)
{
    LY_CHECK_ARG_RET(ctx, ctx,);
    ctx->flags &= ~LY_CTX_PREFER_SEARCHDIRS;
}

API void
ly_ctx_set_allimplemented(struct ly_ctx *ctx)
{
    LY_CHECK_ARG_RET(ctx, ctx,);
    ctx->flags |= LY_CTX_ALLIMPLEMENTED;
}

API void
ly_ctx_unset_allimplemented(struct ly_ctx *ctx)
{
    LY_CHECK_ARG_RET(ctx, ctx,);
    ctx->flags &= ~LY_CTX_ALLIMPLEMENTED;
}

API void
ly_ctx_set_trusted(struct ly_ctx *ctx)
{
    LY_CHECK_ARG_RET(ctx, ctx,);
    ctx->flags |= LY_CTX_TRUSTED;
}

API void
ly_ctx_unset_trusted(struct ly_ctx *ctx)
{
    LY_CHECK_ARG_RET(ctx, ctx,);
    ctx->flags &= ~LY_CTX_TRUSTED;
}

API int
ly_ctx_get_options(struct ly_ctx *ctx)
{
    return ctx->flags;
}

API uint16_t
ly_ctx_get_module_set_id(const struct ly_ctx *ctx)
{
    return ctx->module_set_id;
}

API void
ly_ctx_destroy(struct ly_ctx *ctx, void (*private_destructor)(const struct lysc_node *node, void *priv))
{
    LY_CHECK_ARG_RET(ctx, ctx,);

    /* models list */
    for (; ctx->list.count; ctx->list.count--) {
        /* remove the module */
#if 0 /* TODO when parser implemented */
        lys_free(ctx->list[ctx->list.count - 1], private_destructor, 1, 0);
#endif
    }
    free(ctx->list.objs);

    /* search paths list */
    ly_ctx_unset_searchdirs(ctx, -1);

    /* clean the error list */
    ly_err_clean(ctx, 0);
    pthread_key_delete(ctx->errlist_key);

    /* dictionary */
    lydict_clean(&ctx->dict);

#if 0 /* TODO when plugins implemented */
    /* plugins - will be removed only if this is the last context */
    ly_clean_plugins();
#endif

    free(ctx);
}
