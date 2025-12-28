#ifndef SPIN_LOCK_H
#define SPIN_LOCK_H

#include <atomic>
#ifdef __x86_64__
#include <immintrin.h>
#elif defined(__i386__)
#include <xmmintrin.h>
#elif defined(__arm__)
#include <arm_neon.h>
#elif defined(__aarch64__)
#include <arm_neon.h>
#elif defined(__aarch32__)
#include <arm_neon.h>
#endif

namespace sscl {

/**
 * @brief Simple spinlock using std::atomic
 */
class SpinLock
{
public:
	SpinLock()
	: locked(false)
	{}

	bool tryAcquire()
	{
		bool expected = false;
		return locked.compare_exchange_strong(expected, true);
	}

	inline void spinPause()
	{
#ifdef __x86_64__
		_mm_pause();
#elif defined(__i386__)
		_mm_pause();
#elif defined(__arm__)
		__asm__ volatile("yield");
#elif defined(__aarch64__)
		__asm__ volatile("yield");
#elif defined(__aarch32__)
		__asm__ volatile("yield");
#else
# error "Unsupported architecture"
#endif
	}

	void acquire()
	{
		while (!tryAcquire())
		{
			/**	EXPLANATION:
			 * Busy-wait: keep trying to acquire the lock
			 * The CPU will spin here until the lock becomes available
			 *
			 * The spinPause() function is architecture-specific and is
			 * essential because I once fried an older Intel M-class laptop CPU
			 * when I forgot to include a PAUSE instruction in a for (;;){}
			 * loop. I'm not interested in frying my RPi or my other testbed
			 * robot boards.
			 */
			spinPause();
		}
	}

	void release()
	{
		locked.store(false);
	}

	/**
	 * @brief RAII guard for SpinLock
	 * Locks the spinlock on construction and unlocks on destruction
	 */
	class Guard
	{
	public:
		explicit Guard(SpinLock& lock)
		: lock_(lock), unlocked_(false)
		{
			lock_.acquire();
		}

		~Guard()
		{
			if (!unlocked_) {
				lock_.release();
			}
		}

		void unlockPrematurely()
		{
			if (!unlocked_)
			{
				lock_.release();
				unlocked_ = true;
			}
		}

		// Non-copyable, non-movable
		Guard(const Guard&) = delete;
		Guard& operator=(const Guard&) = delete;
		Guard(Guard&&) = delete;
		Guard& operator=(Guard&&) = delete;

	private:
		SpinLock& lock_;
		bool unlocked_;
	};

private:
	std::atomic<bool> locked;
};

} // namespace sscl

#endif // SPIN_LOCK_H
