#include "config/config_migration.h"

#include "gui/gui.h"

bool MigrateConfigToCurrentVersion(Config& config) {
    const int loadedVersion = config.configVersion;
    const int currentVersion = GetConfigVersion();
    if (loadedVersion >= currentVersion) {
        return false;
    }

    if (loadedVersion < 3) {
        config.fpsLimit = 0;
        config.limitCaptureFramerate = false;
    }

    config.configVersion = currentVersion;
    return true;
}
