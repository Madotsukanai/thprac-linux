#pragma once
#include <windows.h>
namespace THPrac {

enum UpdateState {
    UPDATE_IDLE = 0,        // Not checked yet (or a check was never started).
    UPDATE_CHECKING,        // Background check running.
    UPDATE_AVAILABLE,       // A newer release was found; version/query filled.
    UPDATE_CURRENT,         // The running build is the latest release.
    UPDATE_FAILED,          // The check failed (network, API, parse).
    UPDATE_DOWNLOADING,     // Downloading the update asset.
    UPDATE_INSTALL_FAILED,  // Download or installation failed.
};

// Automatic update against the fork's GitHub Releases:
// https://github.com/Madotsukanai/thprac-linux/releases
//
// StartUpdateCheck queries <latest release> in a background thread and fills
// UpdateQueryStatus with the result. UpdateDownloadAndInstall (UI thread)
// downloads the .exe asset, replaces the running executable and restarts the
// launcher; it only returns when installation failed.
void StartUpdateCheck();
void RestartUpdateCheck();
int UpdateQueryStatus(char* versionBuf, size_t versionBufLen);
void UpdateDownloadAndInstall();
// Remove leftover .old/.update files from an earlier update (call at startup).
void UpdateCleanupTempFiles();

// Updates are manual and use this fork's Releases page in the user's browser.
void OpenReleasePage(HWND window);
}
