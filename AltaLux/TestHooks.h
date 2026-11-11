#pragma once

// Test-only allocation seam. Production code calls ProbeAllocationFailure()
// before each image-sized throwing allocation and ShouldFailAllocation()
// before each _aligned_malloc; both are inert unless a test armed the hook.
// Unit tests arm the hook to inject a single failure (the Nth probed
// allocation, process-wide) and verify that every failure point returns
// false cleanly with the DIB untouched.

#include <atomic>
#include <new>

namespace AltaLuxTest
{
	inline std::atomic<long> g_failAllocationAt{ 0 };
	inline std::atomic<long> g_allocationProbeCount{ 0 };

	/// Arm the hook: the n-th probed allocation (1-based) fails once.
	inline void ArmAllocationFailureAt(long n)
	{
		g_failAllocationAt.store(n, std::memory_order_relaxed);
		g_allocationProbeCount.store(0, std::memory_order_relaxed);
	}

	inline void DisarmAllocationFailure()
	{
		g_failAllocationAt.store(0, std::memory_order_relaxed);
	}

	/// Number of probes seen since the last Arm call (also counts probes made
	/// while armed with an unreachable index, which lets tests assert that a
	/// code path performed no allocation at all).
	inline long AllocationProbeCount()
	{
		return g_allocationProbeCount.load(std::memory_order_relaxed);
	}

	/// Returns true exactly once, when this probe is the armed failure point.
	/// The hook disarms itself after firing so retry paths can recover.
	inline bool ShouldFailAllocation()
	{
		const long failAt = g_failAllocationAt.load(std::memory_order_relaxed);
		if (failAt <= 0)
		{
			return false;
		}
		const long index = g_allocationProbeCount.fetch_add(1, std::memory_order_relaxed) + 1;
		if (index != failAt)
		{
			return false;
		}
		g_failAllocationAt.store(0, std::memory_order_relaxed);
		return true;
	}

	/// Call immediately before a throwing allocation (new[], make_unique,
	/// vector growth). Throws std::bad_alloc when this is the armed point.
	inline void ProbeAllocationFailure()
	{
		if (ShouldFailAllocation())
		{
			throw std::bad_alloc();
		}
	}
}
