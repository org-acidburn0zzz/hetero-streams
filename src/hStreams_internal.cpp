/*
 * Hetero Streams Library - A streaming library for heterogeneous platforms
 * Copyright (c) 2014 - 2016, Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU Lesser General Public License,
 * version 2.1, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for
 * more details.
 */

#include "hStreams_internal.h"
#include "hStreams_exceptions.h"
#include "hStreams_internal_vars_source.h"
#include "hStreams_internal_vars_common.h"
#include "hStreams_locks.h"
#include "hStreams_Logger.h"
#include "hStreams_COIWrapper.h"
#include "hStreams_MKLWrapper.h"
#include <string.h>
#include <stdarg.h>
#ifdef _WIN32
#include <Windows.h>
#endif

#ifdef _WIN32
#define LIB_MKL_RT_NAME "mkl_rt.dll"
#else
#define LIB_MKL_RT_NAME "libmkl_rt.so"
#endif

HSTR_RESULT
hStreams_FetchExecutableName(std::string &out_buff)
{
    char buff[1024];
#ifndef _WIN32
    // This is Linux code.
    ssize_t rv = readlink("/proc/self/exe", buff, (size_t)1024);
#else
    // This is Windows code.
    DWORD rv = GetModuleFileName(NULL, buff, (DWORD)1024);
#endif

    if (rv > 0) {
        if (rv < 1024) {
            buff[rv] = 0;
#ifdef _WIN32
            char fname[_MAX_FNAME];
            _splitpath(buff, NULL/*drive*/, NULL/*dirpath*/, fname, NULL/*extension*/);
            out_buff.assign(fname);
#else
            char *lastSlash = strrchr(buff, '/');
            if (lastSlash) {
                out_buff.assign(lastSlash + 1);
            } else {
                out_buff.assign(buff);
            }
#endif
            return HSTR_RESULT_SUCCESS;
        } else {
            return HSTR_RESULT_BUFF_TOO_SMALL;
        }
    } else {
        return HSTR_RESULT_INTERNAL_ERROR;
    }
}

static LIB_HANDLER::handle_t
hStreams_LoadSingleSinkSideLibraryHost(const char *lib_name)
{
    const std::string full_path = findFileName(lib_name, host_sink_ld_library_path_env_name);
    if (full_path.empty()) {
        throw HSTR_EXCEPTION_MACRO(HSTR_RESULT_BAD_NAME, StringBuilder()
                                   << "Cannot find host sink-side library " << std::string(lib_name)
                                   << " in paths specified by " << host_sink_ld_library_path_env_name << "\n");
    }

    LIB_HANDLER::handle_t handle;
    hStreams_LibLoader::load(full_path, handle);

    HSTR_LOG(HSTR_INFO_TYPE_MISC) << "Loaded host sink-side library: " << full_path;

    return handle;
}

static LIB_HANDLER::handle_t
hStreams_LoadMKLHost()
{
    typedef int (*MKLSettingsHandler_t)(int);
    int mkl_result;
    LIB_HANDLER::handle_t mkl_handle;
    MKLSettingsHandler_t set_threading_layer, set_interface_layer;

    hStreams_LibLoader::load(LIB_MKL_RT_NAME, mkl_handle);
    set_threading_layer = (MKLSettingsHandler_t) hStreams_LibLoader::fetchFunctionAddress(mkl_handle, "MKL_Set_Threading_Layer");
    set_interface_layer = (MKLSettingsHandler_t) hStreams_LibLoader::fetchFunctionAddress(mkl_handle, "MKL_Set_Interface_Layer");

    mkl_result = set_threading_layer(MKL_THREADING_INTEL);

    if (mkl_result != MKL_THREADING_INTEL) {
        HSTR_WARN(HSTR_INFO_TYPE_MISC)
                << "MKL_Set_Threading_Layer returned unexpected value: " << mkl_result
                << ". Expeced value is: MKL_THREADING_INTEL(" << MKL_THREADING_INTEL << ").";
    }

    if (globals::mkl_interface == HSTR_MKL_LP64) {
        mkl_result = set_interface_layer(MKL_INTERFACE_LP64);

        if (mkl_result != MKL_INTERFACE_LP64) {
            HSTR_WARN(HSTR_INFO_TYPE_MISC)
                    << "MKL_Set_Interface_Layer returned unexpected value: " << mkl_result
                    << ". Expeced value is: MKL_INTERFACE_LP64(" << MKL_INTERFACE_LP64 << ").";
        }
    } else if (globals::mkl_interface == HSTR_MKL_ILP64) {
        mkl_result = set_interface_layer(MKL_INTERFACE_ILP64);

        if (mkl_result != MKL_INTERFACE_ILP64) {
            HSTR_WARN(HSTR_INFO_TYPE_MISC)
                    << "MKL_Set_Interface_Layer returned unexpected value: " << mkl_result
                    << ". Expeced value is: MKL_INTERFACE_ILP64(" << MKL_INTERFACE_ILP64 << ").";
        }
    }

    MKLWrapper::cblas_sgemm_handler = (void *) hStreams_LibLoader::fetchFunctionAddress(mkl_handle, "cblas_sgemm");
    MKLWrapper::cblas_dgemm_handler = (void *) hStreams_LibLoader::fetchFunctionAddress(mkl_handle, "cblas_dgemm");
    MKLWrapper::cblas_cgemm_handler = (void *) hStreams_LibLoader::fetchFunctionAddress(mkl_handle, "cblas_cgemm");
    MKLWrapper::cblas_zgemm_handler = (void *) hStreams_LibLoader::fetchFunctionAddress(mkl_handle, "cblas_zgemm");
    return mkl_handle;
}

HSTR_RESULT
hStreams_LoadSinkSideLibrariesHost(std::string const &in_ExecutableFileName, std::vector<LIB_HANDLER::handle_t> &loaded_libs_handles)
{
    HSTR_RESULT hs_result;

    // First, load all of the explicitly specified libraries in the options.
    HSTR_OPTIONS currentOptions = globals::initial_values::options;
    CHECK_HSTR_RESULT(hStreams_GetCurrentOptions(&currentOptions, sizeof(currentOptions)));

    for (uint16_t i = 0; i < currentOptions.libNameCntHost; ++i) {
        const char *lib_name = currentOptions.libNamesHost[i];
        const std::string full_path = findFileName(lib_name, host_sink_ld_library_path_env_name);
        if (full_path.empty()) {
            HSTR_ERROR(HSTR_INFO_TYPE_MISC)
                    << "Cannot find host sink-side library " << lib_name
                    << " in paths specified by " << host_sink_ld_library_path_env_name;

            return HSTR_RESULT_BAD_NAME;
        }
    }

    try {
        for (uint16_t i = 0; i < currentOptions.libNameCntHost; ++i) {
            loaded_libs_handles.push_back(
                hStreams_LoadSingleSinkSideLibraryHost(currentOptions.libNamesHost[i]));
        }

        // Second, load proper MKL version if needed
        if (globals::mkl_interface != HSTR_MKL_NONE) {
            loaded_libs_handles.push_back(
                hStreams_LoadMKLHost());
        }
    } catch (...) {
        return hStreams_handle_exception();
    }

    // Third, load the default sink-side library.
#ifdef _WIN32
    const std::string default_lib = in_ExecutableFileName + "_host.dll";
#else
    const std::string default_lib = in_ExecutableFileName + "_host.so";
#endif
    const std::string full_path = findFileName(default_lib.c_str(), host_sink_ld_library_path_env_name);
    try {
        LIB_HANDLER::handle_t handle;
        hStreams_LibLoader::load(full_path, handle);
        loaded_libs_handles.push_back(handle);
        HSTR_LOG(HSTR_INFO_TYPE_MISC) << "Loaded host sink-side library: " << full_path;

    } catch (hStreams_exception const &e) {
        HSTR_DEBUG1(HSTR_INFO_TYPE_MISC)
                << "Couldn't load default host sink-side library[ " << full_path << "]: " << e.what();
    }
    return HSTR_RESULT_SUCCESS;
}

static HSTR_COILIBRARY
hStreams_LoadSingleSinkSideLibraryMIC(HSTR_COIPROCESS coi_process, const char *lib_name, int libFlags)
{
    const std::string full_path = findFileName(lib_name, mic_sink_ld_library_path_env_name);
    if (full_path.empty()) {
        throw HSTR_EXCEPTION_MACRO(HSTR_RESULT_BAD_NAME, StringBuilder()
                                   << "Cannot load MIC sink-side library " << std::string(lib_name)
                                   << " in paths specified by " << mic_sink_ld_library_path_env_name << "\n");
    }
    HSTR_COILIBRARY coiLibrary;
    HSTR_COIRESULT result = hStreams_COIWrapper::COIProcessLoadLibraryFromFile(
                                coi_process,
                                full_path.c_str(),
                                full_path.c_str(),
                                NULL,
                                libFlags,
                                &coiLibrary);
    if (result == HSTR_COI_SUCCESS) {
        HSTR_LOG(HSTR_INFO_TYPE_MISC) << "Loaded MIC sink-side library " << full_path;
    } else {
        throw HSTR_EXCEPTION_MACRO(HSTR_RESULT_REMOTE_ERROR, StringBuilder()
                                   << "Cannot load MIC sink-side library " << std::string(lib_name)
                                   << ", COI returned: " << std::string(hStreams_COIWrapper::COIResultGetName(result)) << "\n");
    }

    return coiLibrary;
}


HSTR_RESULT
hStreams_LoadSinkSideLibrariesMIC(HSTR_COIPROCESS coi_process, std::vector<HSTR_COILIBRARY> &out_loadedLibs, std::string const &in_ExecutableFileName)
{
    HSTR_RESULT hs_result;
    out_loadedLibs.clear();

    // First, load all of the explicitly specified libraries in the options.
    HSTR_OPTIONS currentOptions = globals::initial_values::options;
    CHECK_HSTR_RESULT(hStreams_GetCurrentOptions(&currentOptions, sizeof(currentOptions)));
    out_loadedLibs.reserve(currentOptions.libNameCnt + 2);

    try {
        for (uint16_t i = 0; i < currentOptions.libNameCnt; ++i) {
            out_loadedLibs.push_back(
                hStreams_LoadSingleSinkSideLibraryMIC(
                    coi_process,
                    currentOptions.libNames[i],
                    currentOptions.libFlags ? currentOptions.libFlags[i] : HSTR_COI_LOADLIBRARY_V1_FLAGS));
        }

        // Second, load proper MKL version if needed
        const char *mkl_name = NULL;
        if (globals::mkl_interface == HSTR_MKL_LP64) {
            mkl_name = "libmkl_intel_lp64.so";
        } else if (globals::mkl_interface == HSTR_MKL_ILP64) {
            mkl_name = "libmkl_intel_ilp64.so";
        }

        if (mkl_name != NULL) {
            // Load dependencies first. libmkl_intel_{,i}lp64.{so,dll} doesn't have a builtin
            // dependency on its actual dependencies so we have to load them manually.
            out_loadedLibs.push_back(
                hStreams_LoadSingleSinkSideLibraryMIC(
                    coi_process,
                    "libmkl_core.so",
                    HSTR_COI_LOADLIBRARY_GLOBAL | HSTR_COI_LOADLIBRARY_LAZY));
            out_loadedLibs.push_back(
                hStreams_LoadSingleSinkSideLibraryMIC(
                    coi_process,
                    "libmkl_intel_thread.so",
                    HSTR_COI_LOADLIBRARY_GLOBAL | HSTR_COI_LOADLIBRARY_LAZY));
            // Load MKL
            out_loadedLibs.push_back(
                hStreams_LoadSingleSinkSideLibraryMIC(
                    coi_process,
                    mkl_name,
                    HSTR_COI_LOADLIBRARY_V1_FLAGS));
        }
    } catch (...) {
        return hStreams_handle_exception();
    }

    // Third, load the default sink-side library.
    const std::string default_lib = in_ExecutableFileName + "_mic.so";
    const std::string full_path = findFileName(default_lib.c_str(), mic_sink_ld_library_path_env_name);
    if (!full_path.empty()) {
        HSTR_COILIBRARY coiLibrary;
        HSTR_COIRESULT result = hStreams_COIWrapper::COIProcessLoadLibraryFromFile(
                                    coi_process,
                                    full_path.c_str(),
                                    full_path.c_str(),
                                    NULL,
                                    HSTR_COI_LOADLIBRARY_V1_FLAGS,
                                    &coiLibrary);
        // We deliberately silently ignore errors for loading the default library.
        if (result == HSTR_COI_SUCCESS) {
            out_loadedLibs.push_back(coiLibrary);

            HSTR_LOG(HSTR_INFO_TYPE_MISC) << "Loaded MIC sink-side library " << full_path;
        } else {
            HSTR_DEBUG1(HSTR_INFO_TYPE_MISC)
                    << "Couldn't load default sink-side library[ " << full_path
                    << "]: " << hStreams_COIWrapper::COIResultGetName(result);
        }
    }
    return HSTR_RESULT_SUCCESS;
}

// Utility function used to find a filename in the path specified by the:
// "SINK_LD_LIBRARY_PATH" env var for mic, or
// "HOST_SINK_LD_LIBRARY_PATH" env var for host.
// FIXME we should really be using something like boost::filesystem or similar for this
std::string findFileName(const char *fileName, const char *env_variable_path)
{
    // If the fileName has any part of a path in it (contains '/'), be it absolute or relative, treat it as
    // an explicitly specified filename, and let it go exactly as supplied:
    if (strchr(fileName, '/')
#ifdef _WIN32
            || strchr(fileName, '\\') || strchr(fileName, ':')
#endif
       ) {
        struct mpss_hstreams_stat st;
        if (!mpss_hstreams_stat(fileName, &st)) {
            return std::string(fileName);
        }
    } else {
        const char *const hstreams_path_env_var = getenv(env_variable_path);

        // If there is no "SINK_LD_LIBRARY_PATH" env var defined, we can not search
        // for the fileName.  So, we summarily return the fileName.
        if (!hstreams_path_env_var) {
            return std::string("");
        } else {
            char *sink_ld_library_path = (char *)alloca(strlen(hstreams_path_env_var) + 1);
            std::string pathSeparator;
            char *str, *saveptr;
            const char *varSeparator;
#ifdef _WIN32
            varSeparator = ";";
            const char backslash = '\\';
            pathSeparator = (((std::string)hstreams_path_env_var).back() == backslash) ? "" : "\\";
#else
            varSeparator = ":";
            pathSeparator = "/";
#endif
            strcpy(sink_ld_library_path, hstreams_path_env_var);
            for (str = sink_ld_library_path;; str = NULL) {
                std::string filepath;
                char *token = mpss_hstreams_strtok(str, varSeparator, &saveptr);
                if (!token) {
                    break;
                }
                filepath.append(token);
                filepath.append(pathSeparator);
                filepath.append(fileName);
                const char *fullPathFileName = filepath.c_str();
                struct mpss_hstreams_stat st;
                if (!mpss_hstreams_stat(fullPathFileName, &st)) {
                    return std::string(fullPathFileName);
                }
            }
        }
    }
    return std::string("");
}

HSTR_LOG_DOM getNextLogDomID()
{
    return (HSTR_LOG_DOM) hStreams_AtomicAdd64(globals::next_log_dom_id, 1);
}

