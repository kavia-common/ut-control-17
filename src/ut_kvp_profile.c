/*
 * UT KVP profile singleton implementation.
 *
 * Motivation:
 * - Some integration tests use ut_kvp_profile_getInstance() as the canonical
 *   source for expected capability lists (e.g., boot supportedResetTypes).
 * - The base ut_kvp API is instance-based; this file provides a safe singleton
 *   wrapper so callers can rely on a shared instance when desired.
 */

#include "ut_kvp_profile.h"

#include <stdlib.h>
#include <string.h>

#include <ut_log.h>

extern ut_kvp_instance_t *gKVP_Instance;

static const char* getProfilePathFromEnv(void)
{
    const char* p = getenv("UT_KVP_PROFILE_PATH");
    if (p && p[0] != '\0')
    {
        return p;
    }
    return NULL;
}

ut_kvp_status_t ut_kvp_profile_loadFromFile(const char* filePath)
{
    if (!filePath || filePath[0] == '\0')
    {
        UT_LOG_ERROR("ut_kvp_profile_loadFromFile: invalid filePath");
        return UT_KVP_STATUS_INVALID_PARAM;
    }

    ut_kvp_instance_t* inst = ut_kvp_createInstance();
    if (!inst)
    {
        UT_LOG_ERROR("ut_kvp_profile_loadFromFile: ut_kvp_createInstance failed");
        return UT_KVP_STATUS_PARSING_ERROR;
    }

    // ut_kvp_open takes mutable char*
    char* mutablePath = strdup(filePath);
    if (!mutablePath)
    {
        ut_kvp_destroyInstance(inst);
        return UT_KVP_STATUS_PARSING_ERROR;
    }

    ut_kvp_status_t st = ut_kvp_open(inst, mutablePath);
    free(mutablePath);

    if (st != UT_KVP_STATUS_SUCCESS)
    {
        ut_kvp_destroyInstance(inst);
        UT_LOG_ERROR("ut_kvp_profile_loadFromFile: ut_kvp_open failed for '%s'", filePath);
        return st;
    }

    // Replace any existing singleton instance.
    if (gKVP_Instance)
    {
        ut_kvp_destroyInstance(gKVP_Instance);
        gKVP_Instance = NULL;
    }

    gKVP_Instance = inst;
    return UT_KVP_STATUS_SUCCESS;
}

ut_kvp_instance_t* ut_kvp_profile_getInstance(void)
{
    if (gKVP_Instance)
    {
        return gKVP_Instance;
    }

    // Lazy-load from env if available. This prevents common "Invalid Handle"
    // failures in integration tests that expect the singleton to exist.
    const char* envPath = getProfilePathFromEnv();
    if (envPath)
    {
        ut_kvp_status_t st = ut_kvp_profile_loadFromFile(envPath);
        if (st == UT_KVP_STATUS_SUCCESS)
        {
            return gKVP_Instance;
        }
        UT_LOG_ERROR("ut_kvp_profile_getInstance: failed to auto-load UT_KVP_PROFILE_PATH='%s'", envPath);
    }

    return NULL;
}

void ut_kvp_profile_release(void)
{
    if (gKVP_Instance)
    {
        ut_kvp_destroyInstance(gKVP_Instance);
        gKVP_Instance = NULL;
    }
}
