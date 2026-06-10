// SPDX-FileCopyrightText: 2026 PS2Recomp Debug Bridge
// SPDX-License-Identifier: MIT
//
// PCSX2 Debug Server — JSON/TCP API wrapping DebugInterface
// Drop this into pcsx2/DebugTools/ and add to CMakeLists.txt
//
// Features:
//   - Full 128-bit register R/W (all 7 categories: GPR, CP0, FPR, FCR, VU0F, VU0I, GSPRIV)
//   - Native PCSX2 disassembly
//   - Expression evaluation with symbol lookup
//   - Conditional breakpoints with expression conditions
//   - Memory watchpoints (read/write/onChange)
//   - Thread list, module list
//   - Pause/Resume/Step execution control
//   - Memory R/W (8/16/32/64/128 bit)
//   - Stack walk

#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <functional>

namespace DebugServer
{
	// Start the debug server on the given TCP port
	// Call this once from PCSX2 initialization (e.g. in VMManager::Initialize)
	void Start(int port = 21512);

	// Stop the server and close all connections
	void Stop();

	// Check if server is running
	bool IsRunning();

	// Called when a breakpoint is hit — wakes up any waiting step/continue
	void OnBreakpointHit();

	// Called once per frame from the CPU/EE thread (VMManager::Internal::VSyncOnCPUThread).
	// Applies pending controller-input injection (pad_set / pad_press commands) to the
	// pad state at the same layer the input recorder uses, so the game sees it as real
	// input on the next SIO2 poll. Near-zero cost when no injection is active.
	void OnVSync();

} // namespace DebugServer
