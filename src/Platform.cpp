#include "Platform.h"
#include "Fs.h"

#include <coreinit/energysaver.h>
#include <coreinit/title.h>
#include <nn/ac.h>
#include <sysapp/launch.h>

namespace platform {

static uint32_t gApdWasEnabled = 0;
static uint32_t gDimWasEnabled = 0;
static bool gAcInit = false;

void init() {
    // Big modpacks take a long time to download and extract. Stop the
    // console from dimming or auto-powering-off in the middle of it.
    IMIsAPDEnabled(&gApdWasEnabled);
    IMIsDimEnabled(&gDimWasEnabled);
    IMDisableAPD();
    IMDisableDim();

    gAcInit = NNResult_IsSuccess(ACInitialize());
}

bool connectNetwork() {
    if (!gAcInit) return false;
    BOOL connected = FALSE;
    if (NNResult_IsSuccess(ACIsApplicationConnected(&connected)) && connected) return true;
    if (NNResult_IsFailure(ACConnect())) {
        fs::log("ACConnect failed; no network configured?");
        return false;
    }
    return true;
}

void shutdown() {
    if (gApdWasEnabled) IMEnableAPD();
    if (gDimWasEnabled) IMEnableDim();
    if (gAcInit) ACFinalize();
    gAcInit = false;
}

bool runningFromHbl() {
    // HBL runs inside Mii Maker (JPN 0x...A000, USA 0x...A100, EUR 0x...A200).
    return (OSGetTitleID() & 0xFFFFFFFFFFFFF0FFull) == 0x000500101004A000ull;
}

void requestExit() {
    if (runningFromHbl()) SYSRelaunchTitle(0, nullptr);
    else SYSLaunchMenu();
}

} // namespace platform
