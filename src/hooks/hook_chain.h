#pragma once


#include <string>

namespace HookChain {

bool TryCreateAndEnableHook(void* target, void* detour, void** outOriginal, const char* what);

bool IsAllowedSwapBuffersThirdPartyHookAddress(const void* addr);

void RefreshAllThirdPartyHookChains();

// The third-party wglSwapBuffers detour Toolscreen is currently chained behind, or nullptr.
void* GetThirdPartyWglSwapBuffersHookTarget();

std::string DescribeAddressWithOwner(const void* addr);

}


