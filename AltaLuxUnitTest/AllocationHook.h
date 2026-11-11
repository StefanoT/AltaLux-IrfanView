#pragma once

// Allocation failure injection for the unit tests. The hook lives in
// AllocationHook.cpp, which overrides global operator new for every
// translation unit compiled into this test DLL — the plugin sources are
// compiled directly into it, so the code under test is intercepted while
// production code stays free of test hooks. Production builds never see
// this file.

namespace AltaLuxTest
{
	/// Arm the hook: the n-th allocation performed through this DLL (1-based)
	/// throws std::bad_alloc. One-shot; counting starts at the Arm call.
	void ArmAllocationFailureAt(long n);

	void DisarmAllocationFailure();

	/// Number of counted allocations since the last Arm call. Also used with
	/// an unreachable index to assert that a code path allocates nothing.
	long AllocationCount();
}
