#include "Config.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace {
    constexpr float kDefaultExteriorStreamingRadius = 80.0f;
    constexpr float kMinimumExteriorStreamingRadius = 40.0f;
    constexpr float kMaximumExteriorStreamingRadius = 400.0f;

    char g_iniPath[MAX_PATH] = {};

    bool BuildIniPath() {
        char executablePath[MAX_PATH] = {};
        const DWORD length = GetModuleFileNameA(nullptr, executablePath, MAX_PATH);
        if (length == 0 || length >= MAX_PATH)
            return false;

        char* separator = std::strrchr(executablePath, '\\');
        if (!separator)
            return false;

        *(separator + 1) = '\0';
        constexpr char kIniFileName[] = "SAInteriorExteriorFix.ini";
        const std::size_t directoryLength = std::strlen(executablePath);
        if (directoryLength + sizeof(kIniFileName) > sizeof(g_iniPath))
            return false;

        std::memcpy(g_iniPath, executablePath, directoryLength);
        std::memcpy(g_iniPath + directoryLength, kIniFileName, sizeof(kIniFileName));
        return true;
    }

    float ReadExteriorStreamingRadius() {
        char value[64] = {};
        const DWORD length = GetPrivateProfileStringA("Streaming", "ExteriorStreamingRadius", "80.0",
            value, static_cast<DWORD>(sizeof(value)), g_iniPath);
        if (length == 0 || length >= sizeof(value) - 1)
            return kDefaultExteriorStreamingRadius;

        errno = 0;
        char* end = nullptr;
        const float parsed = std::strtof(value, &end);
        if (end == value || errno == ERANGE || !std::isfinite(parsed))
            return kDefaultExteriorStreamingRadius;

        while (*end && std::isspace(static_cast<unsigned char>(*end)))
            ++end;
        if (*end != '\0')
            return kDefaultExteriorStreamingRadius;

        return (std::max)(kMinimumExteriorStreamingRadius,
            (std::min)(kMaximumExteriorStreamingRadius, parsed));
    }
}

FixConfig g_config;

void ConfigLoad() {
    g_config.exteriorStreamingRadius = kDefaultExteriorStreamingRadius;
    if (!BuildIniPath())
        return;

    g_config.exteriorStreamingRadius = ReadExteriorStreamingRadius();
}
