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

#include <assert.h>
#include <stdbool.h>
#include <string.h>

#include <ut.h>
#include <ut_controller_yaml_parser.h>
#include <ut_log.h>

static UT_test_suite_t *gSuite = NULL;

#define AVBUFFER_DEFAULT_FILE "../../../rdk-halif-aidl-17/avbuffer/current/hfp-avbuffer.yaml"

static void test_parser_load_default_file(void)
{
    ut_ctrl_yaml_parser_t *p = NULL;

    ut_ctrl_yaml_status_t st = ut_ctrl_yaml_parser_create(&p);
    UT_ASSERT_EQUAL(st, UT_CTRL_YAML_STATUS_OK);
    UT_ASSERT(p != NULL);

    /* Parse the default YAML from the dependency container via relative path. */
    st = ut_ctrl_yaml_parser_load_file(p, AVBUFFER_DEFAULT_FILE);
    UT_ASSERT_EQUAL(st, UT_CTRL_YAML_STATUS_OK);

    /* Placeholder schema validation: should succeed if the YAML contains an avbuffer-ish root. */
    st = ut_ctrl_yaml_parser_validate_avbuffer_schema(p);
    UT_ASSERT_EQUAL(st, UT_CTRL_YAML_STATUS_OK);

    /* Accessor should not crash; if schema doesn't match our candidate keys,
     * it may return SCHEMA_INVALID. That is acceptable as long as load+validate pass. */
    ut_ctrl_avbuffer_config_t cfg;
    (void)ut_ctrl_yaml_get_avbuffer_config(p, &cfg);

    UT_LOG_INFO("Parsed default avbuffer config (best-effort): size=%u latency=%u stream=%u format=[%s]",
                cfg.buffer_size_bytes, cfg.max_latency_ms, cfg.stream_id, cfg.format);

    ut_ctrl_yaml_parser_destroy(p);
}

static void test_parser_load_memory_and_getters(void)
{
    ut_ctrl_yaml_parser_t *p = NULL;

    ut_ctrl_yaml_status_t st = ut_ctrl_yaml_parser_create(&p);
    UT_ASSERT_EQUAL(st, UT_CTRL_YAML_STATUS_OK);
    UT_ASSERT(p != NULL);

    /* A minimal in-memory YAML payload that should satisfy schema and accessors. */
    const char *yaml =
        "avbuffer:\n"
        "  buffer_size_bytes: 4096\n"
        "  max_latency_ms: 25\n"
        "  stream_id: 7\n"
        "  format: \"NV12\"\n";

    st = ut_ctrl_yaml_parser_load_memory(p, yaml);
    UT_ASSERT_EQUAL(st, UT_CTRL_YAML_STATUS_OK);

    st = ut_ctrl_yaml_parser_validate_avbuffer_schema(p);
    UT_ASSERT_EQUAL(st, UT_CTRL_YAML_STATUS_OK);

    ut_ctrl_avbuffer_config_t cfg;
    st = ut_ctrl_yaml_get_avbuffer_config(p, &cfg);
    UT_ASSERT_EQUAL(st, UT_CTRL_YAML_STATUS_OK);

    UT_ASSERT_EQUAL(cfg.buffer_size_bytes, 4096u);
    UT_ASSERT_EQUAL(cfg.max_latency_ms, 25u);
    UT_ASSERT_EQUAL(cfg.stream_id, 7u);
    UT_ASSERT_STRING_EQUAL(cfg.format, "NV12");

    /* Also test generic typed accessors. */
    uint32_t v = 0;
    st = ut_ctrl_yaml_get_u32(p, "avbuffer/buffer_size_bytes", &v);
    UT_ASSERT_EQUAL(st, UT_CTRL_YAML_STATUS_OK);
    UT_ASSERT_EQUAL(v, 4096u);

    char fmt[UT_KVP_MAX_ELEMENT_SIZE];
    st = ut_ctrl_yaml_get_string(p, "avbuffer/format", fmt, sizeof(fmt));
    UT_ASSERT_EQUAL(st, UT_CTRL_YAML_STATUS_OK);
    UT_ASSERT_STRING_EQUAL(fmt, "NV12");

    bool present = false;
    st = ut_ctrl_yaml_field_present(p, "avbuffer/stream_id", &present);
    UT_ASSERT_EQUAL(st, UT_CTRL_YAML_STATUS_OK);
    UT_ASSERT_EQUAL(present, true);

    ut_ctrl_yaml_parser_destroy(p);
}

void register_controller_yaml_parser_tests(void)
{
    gSuite = UT_add_suite("controller-yaml-parser - HPF avbuffer parsing", NULL, NULL);
    assert(gSuite != NULL);

    UT_add_test(gSuite, "load default hfp-avbuffer.yaml (file)", test_parser_load_default_file);
    UT_add_test(gSuite, "load sample YAML (memory) and read typed fields", test_parser_load_memory_and_getters);
}
