#pragma once

struct FixConfig {
    float exteriorStreamingRadius = 80.0f;
};

extern FixConfig g_config;
void ConfigLoad();
