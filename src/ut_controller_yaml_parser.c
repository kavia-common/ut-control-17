/*
 * If not stated otherwise in this file or this component's LICENSE file the
 * following copyright and licenses apply:
 *
 * Copyright 2023-2026 RDK Management
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <ut_controller_yaml_parser.h>

#include <stdlib.h>
#include <string.h>

#include <ut_log.h>

struct ut_ctrl_yaml_parser
{
    ut_kvp_instance_t *kvp;
};

static ut_ctrl_yaml_status_t map_kvp_status(ut_kvp_status_t st)
{
    switch (st)
    {
        case UT_KVP_STATUS_SUCCESS:
            return UT_CTRL_YAML_STATUS_OK;
        case UT_KVP_STATUS_KEY_NOT_FOUND:
            return UT_CTRL_YAML_STATUS_KEY_NOT_FOUND;
        case UT_KVP_STATUS_INVALID_PARAM:
        case UT_KVP_STATUS_NULL_PARAM:
            return UT_CTRL_YAML_STATUS_INVALID_PARAM;
        case UT_KVP_STATUS_NO_DATA:
        case UT_KVP_STATUS_PARSING_ERROR:
        case UT_KVP_STATUS_FILE_OPEN_ERROR:
        case UT_KVP_STATUS_INVALID_INSTANCE:
        default:
            return UT_CTRL_YAML_STATUS_LOAD_FAIL;
    }
}

static bool any_present(ut_ctrl_yaml_parser_t *parser, const char *const *keys, size_t nkeys)
{
    if (!parser || !parser->kvp || !keys)
    {
        return false;
    }

    for (size_t i = 0; i < nkeys; ++i)
    {
        if (keys[i] && ut_kvp_fieldPresent(parser->kvp, keys[i]))
        {
            return true;
        }
    }
    return false;
}

static bool try_get_u32_first(ut_ctrl_yaml_parser_t *parser,
                              const char *const *keys,
                              size_t nkeys,
                              uint32_t *out_value,
                              bool *out_found)
{
    if (out_found)
    {
        *out_found = false;
    }
    if (!parser || !parser->kvp || !keys || !out_value)
    {
        return false;
    }

    for (size_t i = 0; i < nkeys; ++i)
    {
        if (!keys[i])
        {
            continue;
        }

        if (!ut_kvp_fieldPresent(parser->kvp, keys[i]))
        {
            continue;
        }

        uint32_t v = ut_kvp_getUInt32Field(parser->kvp, keys[i]);
        /* Note: ut_kvp_getUInt32Field returns 0 on error. For typical AV buffer fields
         * 0 can be a valid value; so we treat "presence" as "found" and accept 0. */
        *out_value = v;
        if (out_found)
        {
            *out_found = true;
        }
        return true;
    }

    return false;
}

static bool try_get_string_first(ut_ctrl_yaml_parser_t *parser,
                                 const char *const *keys,
                                 size_t nkeys,
                                 char *out_value,
                                 uint32_t out_value_size,
                                 bool *out_found)
{
    if (out_found)
    {
        *out_found = false;
    }
    if (!parser || !parser->kvp || !keys || !out_value || out_value_size == 0)
    {
        return false;
    }

    out_value[0] = '\0';

    for (size_t i = 0; i < nkeys; ++i)
    {
        if (!keys[i])
        {
            continue;
        }

        ut_kvp_status_t st = ut_kvp_getStringField(parser->kvp, keys[i], out_value, out_value_size);
        if (st == UT_KVP_STATUS_SUCCESS)
        {
            if (out_found)
            {
                *out_found = true;
            }
            return true;
        }
    }
    return false;
}

/* ======================================================================
 * Public API
 * ====================================================================== */

// PUBLIC_INTERFACE
ut_ctrl_yaml_status_t ut_ctrl_yaml_parser_create(ut_ctrl_yaml_parser_t **out_parser)
{
    if (!out_parser)
    {
        return UT_CTRL_YAML_STATUS_INVALID_PARAM;
    }

    *out_parser = NULL;

    ut_ctrl_yaml_parser_t *p = (ut_ctrl_yaml_parser_t *)calloc(1, sizeof(*p));
    if (!p)
    {
        return UT_CTRL_YAML_STATUS_ALLOC_FAIL;
    }

    p->kvp = ut_kvp_createInstance();
    if (!p->kvp)
    {
        free(p);
        return UT_CTRL_YAML_STATUS_ALLOC_FAIL;
    }

    *out_parser = p;
    return UT_CTRL_YAML_STATUS_OK;
}

// PUBLIC_INTERFACE
void ut_ctrl_yaml_parser_destroy(ut_ctrl_yaml_parser_t *parser)
{
    if (!parser)
    {
        return;
    }

    if (parser->kvp)
    {
        ut_kvp_destroyInstance(parser->kvp);
        parser->kvp = NULL;
    }

    free(parser);
}

// PUBLIC_INTERFACE
ut_ctrl_yaml_status_t ut_ctrl_yaml_parser_load_file(ut_ctrl_yaml_parser_t *parser, const char *file_path)
{
    if (!parser || !parser->kvp || !file_path)
    {
        return UT_CTRL_YAML_STATUS_INVALID_PARAM;
    }

    ut_kvp_status_t st = ut_kvp_open(parser->kvp, (char *)file_path);
    if (st != UT_KVP_STATUS_SUCCESS)
    {
        UT_LOG_ERROR("Failed to load YAML file [%s] (ut_kvp_open status=%d)", file_path, (int)st);
    }
    return map_kvp_status(st);
}

// PUBLIC_INTERFACE
ut_ctrl_yaml_status_t ut_ctrl_yaml_parser_load_memory(ut_ctrl_yaml_parser_t *parser, const char *yaml_text)
{
    if (!parser || !parser->kvp || !yaml_text)
    {
        return UT_CTRL_YAML_STATUS_INVALID_PARAM;
    }

    /* ut_kvp_openMemory wants a caller-owned mutable buffer; it duplicates internally.
     * We pass a strdup'd copy to avoid surprising callers and keep const API. */
    char *dup = strdup(yaml_text);
    if (!dup)
    {
        return UT_CTRL_YAML_STATUS_ALLOC_FAIL;
    }

    /* Use strlen+1 to include the terminating NUL similar to existing tests. */
    ut_kvp_status_t st = ut_kvp_openMemory(parser->kvp, dup, (uint32_t)(strlen(dup) + 1));
    free(dup);

    if (st != UT_KVP_STATUS_SUCCESS)
    {
        UT_LOG_ERROR("Failed to load YAML from memory (ut_kvp_openMemory status=%d)", (int)st);
    }
    return map_kvp_status(st);
}

// PUBLIC_INTERFACE
ut_ctrl_yaml_status_t ut_ctrl_yaml_parser_validate_avbuffer_schema(ut_ctrl_yaml_parser_t *parser)
{
    if (!parser || !parser->kvp)
    {
        return UT_CTRL_YAML_STATUS_INVALID_PARAM;
    }

    /* Basic placeholder schema:
     * We don't yet enforce the exact HPF schema, but ensure the document contains
     * something that looks like an AV buffer config section.
     *
     * These candidate roots cover common naming variations observed across
     * controller/service configs.
     */
    const char *const roots[] = {
        "avbuffer",
        "avBuffer",
        "av_buffer",
        "hfp/avbuffer",
        "hfp/avBuffer",
        "hfp/av_buffer",
    };

    if (!any_present(parser, roots, sizeof(roots) / sizeof(roots[0])))
    {
        UT_LOG_ERROR("AV buffer schema validation failed: missing expected root (avbuffer/avBuffer/av_buffer)");
        return UT_CTRL_YAML_STATUS_SCHEMA_INVALID;
    }

    /* Optional: validate a couple numeric fields if present. */
    const char *const size_candidates[] = {
        "avbuffer/buffer_size_bytes",
        "avbuffer/bufferSizeBytes",
        "avbuffer/buffer_size",
        "avbuffer/size_bytes",
        "avbuffer/size",
        "hfp/avbuffer/buffer_size_bytes",
    };

    bool present = false;
    (void)try_get_u32_first(parser, size_candidates, sizeof(size_candidates) / sizeof(size_candidates[0]),
                            &(uint32_t){0}, &present);
    if (present)
    {
        /* Here we could enforce constraints (e.g., >0). Keep as placeholder. */
        UT_LOG_DEBUG("AV buffer schema: buffer size present");
    }

    return UT_CTRL_YAML_STATUS_OK;
}

// PUBLIC_INTERFACE
ut_ctrl_yaml_status_t ut_ctrl_yaml_get_u32(ut_ctrl_yaml_parser_t *parser, const char *key, uint32_t *out_value)
{
    if (!parser || !parser->kvp || !key || !out_value)
    {
        return UT_CTRL_YAML_STATUS_INVALID_PARAM;
    }

    if (!ut_kvp_fieldPresent(parser->kvp, key))
    {
        return UT_CTRL_YAML_STATUS_KEY_NOT_FOUND;
    }

    *out_value = ut_kvp_getUInt32Field(parser->kvp, key);
    return UT_CTRL_YAML_STATUS_OK;
}

// PUBLIC_INTERFACE
ut_ctrl_yaml_status_t ut_ctrl_yaml_get_string(ut_ctrl_yaml_parser_t *parser,
                                              const char *key,
                                              char *out_value,
                                              uint32_t out_value_size)
{
    if (!parser || !parser->kvp || !key || !out_value || out_value_size == 0)
    {
        return UT_CTRL_YAML_STATUS_INVALID_PARAM;
    }

    ut_kvp_status_t st = ut_kvp_getStringField(parser->kvp, key, out_value, out_value_size);
    return map_kvp_status(st);
}

// PUBLIC_INTERFACE
ut_ctrl_yaml_status_t ut_ctrl_yaml_field_present(ut_ctrl_yaml_parser_t *parser, const char *key, bool *out_present)
{
    if (!parser || !parser->kvp || !key || !out_present)
    {
        return UT_CTRL_YAML_STATUS_INVALID_PARAM;
    }

    *out_present = ut_kvp_fieldPresent(parser->kvp, key);
    return UT_CTRL_YAML_STATUS_OK;
}

// PUBLIC_INTERFACE
ut_ctrl_yaml_status_t ut_ctrl_yaml_get_avbuffer_config(ut_ctrl_yaml_parser_t *parser, ut_ctrl_avbuffer_config_t *out_cfg)
{
    if (!parser || !parser->kvp || !out_cfg)
    {
        return UT_CTRL_YAML_STATUS_INVALID_PARAM;
    }

    memset(out_cfg, 0, sizeof(*out_cfg));

    bool any_field = false;

    /* Buffer size */
    {
        const char *const keys[] = {
            "avbuffer/buffer_size_bytes",
            "avbuffer/bufferSizeBytes",
            "avbuffer/buffer_size",
            "avbuffer/size_bytes",
            "avbuffer/size",
            "hfp/avbuffer/buffer_size_bytes",
        };
        bool found = false;
        (void)try_get_u32_first(parser, keys, sizeof(keys) / sizeof(keys[0]), &out_cfg->buffer_size_bytes, &found);
        any_field = any_field || found;
    }

    /* Max latency */
    {
        const char *const keys[] = {
            "avbuffer/max_latency_ms",
            "avbuffer/maxLatencyMs",
            "avbuffer/latency_ms",
            "avbuffer/latency",
            "hfp/avbuffer/max_latency_ms",
        };
        bool found = false;
        (void)try_get_u32_first(parser, keys, sizeof(keys) / sizeof(keys[0]), &out_cfg->max_latency_ms, &found);
        any_field = any_field || found;
    }

    /* Stream id */
    {
        const char *const keys[] = {
            "avbuffer/stream_id",
            "avbuffer/streamId",
            "avbuffer/stream",
            "hfp/avbuffer/stream_id",
        };
        bool found = false;
        (void)try_get_u32_first(parser, keys, sizeof(keys) / sizeof(keys[0]), &out_cfg->stream_id, &found);
        any_field = any_field || found;
    }

    /* Format */
    {
        const char *const keys[] = {
            "avbuffer/format",
            "avbuffer/buffer_format",
            "avbuffer/bufferFormat",
            "hfp/avbuffer/format",
        };
        bool found = false;
        (void)try_get_string_first(parser, keys, sizeof(keys) / sizeof(keys[0]),
                                   out_cfg->format, (uint32_t)sizeof(out_cfg->format), &found);
        any_field = any_field || found;
    }

    return any_field ? UT_CTRL_YAML_STATUS_OK : UT_CTRL_YAML_STATUS_SCHEMA_INVALID;
}
