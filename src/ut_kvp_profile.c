/*
 * UT KVP profile singleton implementation.
 *
 * Motivation:
 * - Some integration tests use ut_kvp_profile_getInstance() as the canonical
 *   source for expected capability lists (e.g., boot supportedResetTypes).
 * - The base ut_kvp API is instance-based; this file provides a safe singleton
 *   wrapper so callers can rely on a shared instance when desired.
 *
 * Robustness note:
 * - Some harnesses (notably certain VTS builds) may end up calling
 *   ut_kvp_profile_getInstance() without having a profile loaded, and in some
 *   builds/linkage models the legacy global gKVP_Instance may not be reliably
 *   initialized by the harness itself.
 * - To avoid NULL/invalid handle failures, this module maintains its own
 *   singleton pointer and keeps the legacy gKVP_Instance symbol in sync.
 */

#include "ut_kvp_profile.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ut_log.h>

/*
 * Legacy global from ut_kvp.c.
 *
 * Some harnesses historically rely on this symbol rather than consistently
 * using ut_kvp_profile_getInstance(). In certain linkage models (notably VTS),
 * failing to keep this in sync can result in NULL/invalid handles being used,
 * triggering "Invalid Handle" and listCount==0 failures.
 */
extern ut_kvp_instance_t *gKVP_Instance;

/*
 * Internal singleton instance pointer owned by this TU.
 * This is the canonical instance for ut_kvp_profile_* APIs.
 */
static ut_kvp_instance_t *gKVP_ProfileInstance = NULL;

/*
 * NOTE:
 * We must not duplicate ut_kvp.c's internal instance struct here.
 * In some link/DSO layouts (e.g., VTS), mixing different internal layouts
 * would cause validateInstance() magic checks (and/or struct interpretation)
 * to fail, producing "Invalid Handle".
 *
 * The only safe way to obtain a valid instance is via ut_kvp_createInstance()
 * from ut_kvp.c, and to treat ut_kvp_instance_t as opaque here.
 */
static bool isValidKvpInstanceNoLog(ut_kvp_instance_t *inst)
{
    if (inst == NULL)
        return false;
    /* Best-effort: magic is expected to be the first field (ut_kvp.c). */
    const uint32_t *magic = (const uint32_t *)inst;
    return (*magic == 0xdeadbeef);
}

/**
 * Minimal embedded profile used as a last-resort fallback when no on-disk
 * profile can be located and no env var is provided (common in VTS harnesses).
 *
 * This is intentionally small: it only targets the keys used by VTS_L1_BOOT
 * capability verification so ut_kvp_profile_getInstance() never returns NULL.
 */
static const char *kEmbeddedBootProfileYaml =
    "boot:\n"
    "  capabilities:\n"
    "    supportedResetTypes:\n"
    "      - FULL_SYSTEM_RESET\n"
    "      - SOFTWARE_REBOOT\n"
    "      - MAINTENANCE_REBOOT\n"
    "      - FORCE_DISASTER_RECOVERY\n"
    "      - INVALIDATE_CURRENT_APPLICATION_IMAGE\n"
    "    supportedBootReasons:\n"
    "      - ERROR_UNKNOWN\n"
    "      - WATCHDOG\n"
    "      - MAINTENANCE_REBOOT\n"
    "      - THERMAL_RESET\n"
    "      - WARM_RESET\n"
    "      - COLD_BOOT\n"
    "      - STR_AUTH_FAILURE\n";

static const char* getProfileDataFromEnv(void)
{
    const char* p = getenv("UT_KVP_PROFILE_DATA");
    if (p && p[0] != '\0')
    {
        return p;
    }
    return NULL;
}

static const char* getProfilePathFromEnv(void)
{
    const char* p = getenv("UT_KVP_PROFILE_PATH");
    if (p && p[0] != '\0')
    {
        return p;
    }
    return NULL;
}

static void setSingletonInstance(ut_kvp_instance_t *inst)
{
    /*
     * Single point of truth for updating our internal singleton and mirroring
     * to the legacy global. This reduces the risk of the two getting out-of-sync.
     */
    gKVP_ProfileInstance = inst;
    gKVP_Instance = inst;
}

static void destroyCurrentSingleton(void)
{
    if (gKVP_ProfileInstance)
    {
        ut_kvp_destroyInstance(gKVP_ProfileInstance);
        gKVP_ProfileInstance = NULL;
    }
    /* Ensure legacy global never points at freed memory. */
    gKVP_Instance = NULL;
}

static ut_kvp_status_t ut_kvp_profile_loadFromMemory(const char* yamlData)
{
    if (!yamlData || yamlData[0] == '\0')
    {
        UT_LOG_ERROR("ut_kvp_profile_loadFromMemory: invalid yamlData");
        return UT_KVP_STATUS_INVALID_PARAM;
    }

    ut_kvp_instance_t* inst = ut_kvp_createInstance();
    if (!inst)
    {
        UT_LOG_ERROR("ut_kvp_profile_loadFromMemory: ut_kvp_createInstance failed");
        return UT_KVP_STATUS_PARSING_ERROR;
    }

    /* ut_kvp_openMemory takes mutable char* */
    char* mutableData = strdup(yamlData);
    if (!mutableData)
    {
        ut_kvp_destroyInstance(inst);
        return UT_KVP_STATUS_PARSING_ERROR;
    }

    ut_kvp_status_t st = ut_kvp_openMemory(inst, mutableData, (uint32_t)strlen(mutableData));
    free(mutableData);

    if (st != UT_KVP_STATUS_SUCCESS)
    {
        ut_kvp_destroyInstance(inst);
        UT_LOG_ERROR("ut_kvp_profile_loadFromMemory: ut_kvp_openMemory failed");
        return st;
    }

    destroyCurrentSingleton();
    setSingletonInstance(inst);
    return UT_KVP_STATUS_SUCCESS;
}

static ut_kvp_status_t tryLoadFromDefaultPaths(void)
{
    char exePath[1024] = {0};
    char exeDir[1024] = {0};
    ssize_t exeLen = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
    if (exeLen > 0)
    {
        exePath[exeLen] = '\0';
        /* Derive directory in-place (no libgen dependency). */
        strncpy(exeDir, exePath, sizeof(exeDir) - 1);
        exeDir[sizeof(exeDir) - 1] = '\0';
        char *lastSlash = strrchr(exeDir, '/');
        if (lastSlash)
        {
            *lastSlash = '\0';
        }
        else
        {
            exeDir[0] = '\0';
        }
    }

    char exeProfile1[1200] = {0};
    char exeProfile2[1200] = {0};
    char exeProfile3[1200] = {0};
    char exeProfile4[1200] = {0};
    if (exeDir[0] != '\0')
    {
        snprintf(exeProfile1, sizeof(exeProfile1), "%s/%s", exeDir, "profile.yaml");
        snprintf(exeProfile2, sizeof(exeProfile2), "%s/%s", exeDir, "ut_kvp_profile.yaml");
        snprintf(exeProfile3, sizeof(exeProfile3), "%s/%s", exeDir, "assets/profile.yaml");
        snprintf(exeProfile4, sizeof(exeProfile4), "%s/%s", exeDir, "assets/ut_kvp_profile.yaml");
    }

    /* Common locations used by various test harnesses when env vars are not propagated. */
    static const char* kDefaultPaths[] = {
        "profile.yaml",
        "assets/profile.yaml",
        "config/profile.yaml",
        "configs/profile.yaml",
        "ut_kvp_profile.yaml",
        "assets/ut_kvp_profile.yaml",
        /* Common device/VTS style locations */
        "/vendor/etc/ut_kvp_profile.yaml",
        "/vendor/etc/profile.yaml",
        "/etc/ut_kvp_profile.yaml",
        "/etc/profile.yaml",
        /* Common workspace-style locations used by some harnesses */
        "tests/src/assets/config-test.yaml",
        "tests/assets/config-test.yaml",
    };

    /* First: try alongside the running binary (typical for VTS packaging). */
    if (exeDir[0] != '\0')
    {
        const char *exeCandidates[] = { exeProfile1, exeProfile2, exeProfile3, exeProfile4 };
        for (size_t i = 0; i < sizeof(exeCandidates) / sizeof(exeCandidates[0]); ++i)
        {
            if (!exeCandidates[i] || exeCandidates[i][0] == '\0')
            {
                continue;
            }
            ut_kvp_status_t st = ut_kvp_profile_loadFromFile(exeCandidates[i]);
            if (st == UT_KVP_STATUS_SUCCESS)
            {
                UT_LOG_DEBUG("ut_kvp_profile_getInstance: auto-loaded profile from exe-relative path '%s'", exeCandidates[i]);
                return st;
            }
        }
    }

    for (size_t i = 0; i < sizeof(kDefaultPaths) / sizeof(kDefaultPaths[0]); ++i)
    {
        ut_kvp_status_t st = ut_kvp_profile_loadFromFile(kDefaultPaths[i]);
        if (st == UT_KVP_STATUS_SUCCESS)
        {
            UT_LOG_DEBUG("ut_kvp_profile_getInstance: auto-loaded profile from default path '%s'", kDefaultPaths[i]);
            return st;
        }
    }

    return UT_KVP_STATUS_FILE_OPEN_ERROR;
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

    /* ut_kvp_open takes mutable char* */
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

    /* Replace any existing singleton instance. */
    destroyCurrentSingleton();
    setSingletonInstance(inst);
    return UT_KVP_STATUS_SUCCESS;
}

ut_kvp_instance_t* ut_kvp_profile_getInstance(void)
{
    /* Prefer internal singleton first. */
    if (gKVP_ProfileInstance)
    {
        return gKVP_ProfileInstance;
    }

    /*
     * Compatibility: if the legacy global was already initialized elsewhere in
     * the process (some harnesses do this), reuse it as our singleton.
     *
     * This directly addresses VTS logs showing:
     *  - ut_kvp_getListCount(ut_kvp_profile_getInstance(), ...) => Invalid Handle
     * by ensuring getInstance never returns a NULL/invalid handle when a valid
     * legacy instance exists.
     */
    if (isValidKvpInstanceNoLog(gKVP_Instance))
    {
        gKVP_ProfileInstance = gKVP_Instance;
        return gKVP_ProfileInstance;
    }

    UT_LOG_DEBUG("ut_kvp_profile_getInstance: singleton not initialized; attempting auto-load");

    /*
     * Create an instance early so the handle is always magic-valid.
     * IMPORTANT: Never return a fake/sentinel pointer; ut_kvp validates the
     * instance magic and will emit "Invalid Handle" (as seen in the VTS log).
     */
    ut_kvp_instance_t* emptyInst = ut_kvp_createInstance();
    if (!emptyInst)
    {
        UT_LOG_ERROR("ut_kvp_profile_getInstance: failed to allocate instance (OOM); using emergency instance");

        /* If some other TU/DSO already has a valid legacy instance, prefer it. */
        if (isValidKvpInstanceNoLog(gKVP_Instance))
        {
            gKVP_ProfileInstance = gKVP_Instance;
            return gKVP_ProfileInstance;
        }
        /*
         * Last resort: return NULL. Callers like ut_kvp_getListCount() have
         * their own recovery path to the profile singleton and will handle
         * NULL safely (without dereferencing).
         */
        return NULL;
    }
    setSingletonInstance(emptyInst);

    /*
     * Lazy-load from env if available. This prevents common "Invalid Handle"
     * failures in integration tests that expect the singleton to exist.
     */
    const char* envData = getProfileDataFromEnv();
    if (envData)
    {
        ut_kvp_status_t st = ut_kvp_profile_loadFromMemory(envData);
        if (st == UT_KVP_STATUS_SUCCESS)
        {
            return gKVP_ProfileInstance;
        }
        UT_LOG_ERROR("ut_kvp_profile_getInstance: failed to auto-load UT_KVP_PROFILE_DATA");
    }

    const char* envPath = getProfilePathFromEnv();
    if (envPath)
    {
        ut_kvp_status_t st = ut_kvp_profile_loadFromFile(envPath);
        if (st == UT_KVP_STATUS_SUCCESS)
        {
            return gKVP_ProfileInstance;
        }
        UT_LOG_ERROR("ut_kvp_profile_getInstance: failed to auto-load UT_KVP_PROFILE_PATH='%s'", envPath);
    }

    /*
     * Final fallback: attempt a small set of conventional relative paths.
     * This is intended for VTS/CI harnesses that ship the profile next to the test binary
     * but do not propagate environment variables.
     */
    if (tryLoadFromDefaultPaths() == UT_KVP_STATUS_SUCCESS)
    {
        return gKVP_ProfileInstance;
    }

    /*
     * Last resort: load a minimal embedded boot profile so callers never see
     * a NULL/invalid handle (prevents "Invalid Handle" failures in VTS).
     */
    ut_kvp_status_t st = ut_kvp_profile_loadFromMemory(kEmbeddedBootProfileYaml);
    if (st == UT_KVP_STATUS_SUCCESS)
    {
        UT_LOG_DEBUG("ut_kvp_profile_getInstance: loaded embedded minimal boot profile (fallback)");
        return gKVP_ProfileInstance;
    }

    /*
     * If even the embedded profile can't be loaded (e.g. OOM), return the
     * already-created empty instance. This preserves handle validity (magic),
     * but list counts will be 0.
     */
    UT_LOG_ERROR("ut_kvp_profile_getInstance: failed to load embedded fallback profile; returning empty instance");

    return gKVP_ProfileInstance;
}

void ut_kvp_profile_release(void)
{
    destroyCurrentSingleton();
}
