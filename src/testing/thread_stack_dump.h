#pragma once

#include <string>

namespace GameTest {

// Samples the native stack of every thread in the process (except the caller) and returns a symbolized
// report of the threads that have a frame in one of the interesting modules (Toolscreen, the Vulkan
// loader, OBS's hook, GPU drivers). Used to diagnose hangs found by the in-game tests. x64 only.
std::string DumpInterestingThreadStacks();

} // namespace GameTest
