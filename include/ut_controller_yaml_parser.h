#ifndef UT_CONTROLLER_YAML_PARSER_H
#define UT_CONTROLLER_YAML_PARSER_H

#include <stdbool.h>
#include <stdint.h>

#include <ut_kvp.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Status codes for the controller YAML parser utility.
 *
 * This module is a thin wrapper around ut-control's KVP/YAML APIs (ut_kvp_*),
 * providing controller-friendly load, validate, and typed accessor functions
 * for HPF (HAL Profile Format) YAML payloads used by AV buffer configuration.
 */
typedef enum
{
    UT_CTRL_YAML_STATUS_OK = 0,              /**< Operation successful. */
    UT_CTRL_YAML_STATUS_INVALID_PARAM,       /**< Invalid parameter passed. */
    UT_CTRL_YAML_STATUS_ALLOC_FAIL,          /**< Allocation failure. */
    UT_CTRL_YAML_STATUS_LOAD_FAIL,           /**< Underlying YAML/KVP load failure. */
    UT_CTRL_YAML_STATUS_SCHEMA_INVALID,      /**< Missing required key or type mismatch. */
    UT_CTRL_YAML_STATUS_KEY_NOT_FOUND,       /**< Key not found. */
    UT_CTRL_YAML_STATUS_VALUE_INVALID        /**< Key found but value invalid/out of range. */
} ut_ctrl_yaml_status_t;

/**
 * @brief Opaque handle for a controller YAML parser instance.
 *
 * Internally contains a ut_kvp_instance_t and optional metadata used for
 * validation and debugging.
 */
typedef struct ut_ctrl_yaml_parser ut_ctrl_yaml_parser_t;

/**
 * @brief Common AV buffer fields extracted from HPF YAML.
 *
 * This is intentionally small and focused; additional fields can be added
 * incrementally without changing the underlying parsing mechanism.
 */
typedef struct
{
    uint32_t buffer_size_bytes;     /**< Commonly used AV buffer size in bytes. */
    uint32_t max_latency_ms;        /**< Maximum allowed latency in milliseconds. */
    uint32_t stream_id;             /**< Stream identifier (if provided). */
    char format[UT_KVP_MAX_ELEMENT_SIZE]; /**< Buffer/payload format string (if provided). */
} ut_ctrl_avbuffer_config_t;

/* ======================================================================
 * Lifecycle / Loading
 * ====================================================================== */

/**
 * PUBLIC_INTERFACE
 * @brief Create a controller YAML parser instance.
 *
 * @param[out] out_parser Receives a newly allocated parser handle on success.
 * @return ut_ctrl_yaml_status_t
 */
ut_ctrl_yaml_status_t ut_ctrl_yaml_parser_create(ut_ctrl_yaml_parser_t **out_parser);

/**
 * PUBLIC_INTERFACE
 * @brief Destroy a controller YAML parser instance and free resources.
 *
 * Safe to call with NULL.
 *
 * @param[in,out] parser Parser to destroy (may be NULL).
 */
void ut_ctrl_yaml_parser_destroy(ut_ctrl_yaml_parser_t *parser);

/**
 * PUBLIC_INTERFACE
 * @brief Load/merge HPF YAML into this parser from a file path (supports !include).
 *
 * This wraps ut_kvp_open(), which supports include semantics and document merging.
 *
 * @param[in,out] parser Parser instance.
 * @param[in] file_path Path to YAML file.
 * @return ut_ctrl_yaml_status_t
 */
ut_ctrl_yaml_status_t ut_ctrl_yaml_parser_load_file(ut_ctrl_yaml_parser_t *parser, const char *file_path);

/**
 * PUBLIC_INTERFACE
 * @brief Load/merge HPF YAML into this parser from an in-memory YAML string.
 *
 * This wraps ut_kvp_openMemory(). Note: ut_kvp_openMemory() supports include
 * semantics inside the YAML, but local-file includes are resolved relative to
 * the current working directory (same behavior as ut_kvp).
 *
 * @param[in,out] parser Parser instance.
 * @param[in] yaml_text NUL-terminated YAML string (caller-owned).
 * @return ut_ctrl_yaml_status_t
 */
ut_ctrl_yaml_status_t ut_ctrl_yaml_parser_load_memory(ut_ctrl_yaml_parser_t *parser, const char *yaml_text);

/**
 * PUBLIC_INTERFACE
 * @brief Validate presence and basic type of required HPF AV buffer keys.
 *
 * This is a “basic schema placeholder” validation. It checks that at least one of a
 * small set of known AV buffer root paths exists, and optionally validates a few
 * scalar numeric fields (if present).
 *
 * @param[in] parser Parser instance.
 * @return ut_ctrl_yaml_status_t
 */
ut_ctrl_yaml_status_t ut_ctrl_yaml_parser_validate_avbuffer_schema(ut_ctrl_yaml_parser_t *parser);

/* ======================================================================
 * Generic typed accessors
 * ====================================================================== */

/**
 * PUBLIC_INTERFACE
 * @brief Get a uint32 value at a YAML/KVP path.
 *
 * @param[in] parser Parser instance.
 * @param[in] key KVP key path (slash or dot paths supported by ut_kvp).
 * @param[out] out_value Parsed value.
 * @return ut_ctrl_yaml_status_t
 */
ut_ctrl_yaml_status_t ut_ctrl_yaml_get_u32(ut_ctrl_yaml_parser_t *parser, const char *key, uint32_t *out_value);

/**
 * PUBLIC_INTERFACE
 * @brief Get a string value at a YAML/KVP path.
 *
 * @param[in] parser Parser instance.
 * @param[in] key KVP key path.
 * @param[out] out_value Output buffer.
 * @param[in] out_value_size Size of output buffer in bytes.
 * @return ut_ctrl_yaml_status_t
 */
ut_ctrl_yaml_status_t ut_ctrl_yaml_get_string(ut_ctrl_yaml_parser_t *parser,
                                              const char *key,
                                              char *out_value,
                                              uint32_t out_value_size);

/**
 * PUBLIC_INTERFACE
 * @brief Test whether a key exists in the parsed document.
 *
 * @param[in] parser Parser instance.
 * @param[in] key KVP key path.
 * @param[out] out_present True if present.
 * @return ut_ctrl_yaml_status_t
 */
ut_ctrl_yaml_status_t ut_ctrl_yaml_field_present(ut_ctrl_yaml_parser_t *parser, const char *key, bool *out_present);

/* ======================================================================
 * AVBuffer-specific helpers
 * ====================================================================== */

/**
 * PUBLIC_INTERFACE
 * @brief Attempt to populate an AV buffer config structure using common key candidates.
 *
 * This does not require a single fixed schema; it tries multiple candidate keys
 * for each field and returns OK if at least one field is found. Missing fields
 * remain default (0 / empty string).
 *
 * @param[in] parser Parser instance.
 * @param[out] out_cfg Output config structure.
 * @return ut_ctrl_yaml_status_t
 */
ut_ctrl_yaml_status_t ut_ctrl_yaml_get_avbuffer_config(ut_ctrl_yaml_parser_t *parser, ut_ctrl_avbuffer_config_t *out_cfg);

#ifdef __cplusplus
}
#endif

#endif /* UT_CONTROLLER_YAML_PARSER_H */
