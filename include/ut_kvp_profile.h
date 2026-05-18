#pragma once
/*
 * UT KVP Profile Singleton
 *
 * Some integration/VTS tests expect a process-wide "profile instance" accessible
 * via ut_kvp_profile_getInstance(). The base ut_kvp API is instance-based, but
 * does not itself define any singleton behavior.
 *
 * This header exposes a minimal singleton API:
 *  - ut_kvp_profile_getInstance(): returns the current profile instance (may be NULL
 *    if not loaded and auto-load is disabled).
 *  - ut_kvp_profile_loadFromFile(path): create/open and set the singleton instance.
 *  - ut_kvp_profile_release(): destroy and clear the singleton instance.
 *
 * Auto-load behavior:
 *  - If UT_KVP_PROFILE_PATH is set in the environment, the first call to
 *    ut_kvp_profile_getInstance() will attempt to load it automatically.
 *
 * This keeps existing code working while allowing tests to reliably initialize
 * expected-data lookups.
 */

#include <ut_kvp.h>

#ifdef __cplusplus
extern "C" {
#endif

// PUBLIC_INTERFACE
ut_kvp_instance_t* ut_kvp_profile_getInstance(void);
/**
 * Return the global profile instance.
 *
 * If no instance has been loaded yet and the environment variable
 * UT_KVP_PROFILE_PATH is set, this function will lazily load the profile from
 * that YAML file.
 *
 * @return ut_kvp_instance_t* valid instance on success, or NULL on failure.
 */

// PUBLIC_INTERFACE
ut_kvp_status_t ut_kvp_profile_loadFromFile(const char* filePath);
/**
 * Create/open a KVP instance from filePath and set it as the global profile.
 *
 * If an instance was previously loaded, it will be destroyed and replaced.
 *
 * @param filePath Path to the YAML profile.
 * @return UT_KVP_STATUS_SUCCESS on success; appropriate error status otherwise.
 */

// PUBLIC_INTERFACE
void ut_kvp_profile_release(void);
/**
 * Destroy and clear the global profile instance.
 */

#ifdef __cplusplus
}
#endif
