// Global operator new override for allocation-failure injection; see
// AllocationHook.h. The overrides are symmetric (new -> malloc, delete ->
// free) and replace the CRT's operator new/delete for the whole test DLL,
// so every allocation made by the plugin code under test passes through the
// hook. Other modules in the process (testhost, framework) are unaffected.

#include "AllocationHook.h"

#include <atomic>
#include <cstdlib>
#include <new>

namespace AltaLuxTest
{
	namespace
	{
		std::atomic<long> g_failAllocationAt{ 0 };
		std::atomic<long> g_allocationCount{ 0 };

		// Returns true exactly once, when this allocation is the armed point;
		// the hook disarms itself after firing so retry paths can recover.
		bool ConsumeFailure()
		{
			const long failAt = g_failAllocationAt.load(std::memory_order_relaxed);
			if (failAt <= 0)
			{
				return false;
			}
			const long index = g_allocationCount.fetch_add(1, std::memory_order_relaxed) + 1;
			if (index != failAt)
			{
				return false;
			}
			g_failAllocationAt.store(0, std::memory_order_relaxed);
			return true;
		}

		void* AllocateOrThrow(std::size_t size)
		{
			if (ConsumeFailure())
			{
				throw std::bad_alloc();
			}
			void* block = std::malloc(size == 0 ? 1 : size);
			if (block == nullptr)
			{
				throw std::bad_alloc();
			}
			return block;
		}
	}

	void ArmAllocationFailureAt(long n)
	{
		g_failAllocationAt.store(n, std::memory_order_relaxed);
		g_allocationCount.store(0, std::memory_order_relaxed);
	}

	void DisarmAllocationFailure()
	{
		g_failAllocationAt.store(0, std::memory_order_relaxed);
	}

	long AllocationCount()
	{
		return g_allocationCount.load(std::memory_order_relaxed);
	}
}

void* operator new(std::size_t size)
{
	return AltaLuxTest::AllocateOrThrow(size);
}

void* operator new[](std::size_t size)
{
	return AltaLuxTest::AllocateOrThrow(size);
}

void operator delete(void* block) noexcept
{
	std::free(block);
}

void operator delete[](void* block) noexcept
{
	std::free(block);
}

void operator delete(void* block, std::size_t) noexcept
{
	std::free(block);
}

void operator delete[](void* block, std::size_t) noexcept
{
	std::free(block);
}
