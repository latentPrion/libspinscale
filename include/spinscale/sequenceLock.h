#ifndef SPINSCALE_SEQUENCE_LOCK_H
#define SPINSCALE_SEQUENCE_LOCK_H

#include <atomic>
#include <optional>

namespace sscl {

/**
 * @brief Sequence lock synchronization primitive
 *
 * A reader-writer synchronization primitive where writers increment the
 * sequence number (odd = writing in progress, even = stable) and readers
 * check the sequence number to detect concurrent modifications.
 */
class SequenceLock
{
public:
	SequenceLock()
	: sequenceNo(0)
	{}

	~SequenceLock() = default;

	// Non-copyable, non-movable (std::atomic is neither copyable nor movable)
	SequenceLock(const SequenceLock&) = delete;
	SequenceLock& operator=(const SequenceLock&) = delete;
	SequenceLock(SequenceLock&&) = delete;
	SequenceLock& operator=(SequenceLock&&) = delete;

	/* Atomically increments sequenceNo and issues a release barrier.
	 * Makes the sequence number odd, indicating a write is in progress.
	 */
	void writeAcquire()
		{ sequenceNo.fetch_add(1, std::memory_order_release); }

	/* Atomically increments sequenceNo and issues a release barrier.
	 * Makes the sequence number even again, indicating write is complete.
	 */
	void writeRelease()
		{ sequenceNo.fetch_add(1, std::memory_order_release); }

	/* Issues an acquire barrier and checks if the sequence number is even
	 * (stable state). If odd (writer active), returns nullopt. Otherwise
	 * returns the sequence number.
	 *
	 * @return std::nullopt if writer is active, otherwise the sequence number
	 */
	std::optional<size_t> readAcquire()
	{
		size_t seq = sequenceNo.load(std::memory_order_acquire);
		if (seq & 1) {
			return std::nullopt;
		}
		return seq;
	}

	/* Issues an acquire barrier and checks if the sequence number matches
	 * the original value from readAcquire(). If equal, the read was consistent.
	 *
	 * @param originalSequenceNo The sequence number obtained from readAcquire()
	 * @return true if read was consistent, false if writer modified during read
	 */
	bool readRelease(size_t originalSequenceNo)
	{
		size_t seq = sequenceNo.load(std::memory_order_acquire);
		return seq == originalSequenceNo;
	}

private:
	std::atomic<size_t> sequenceNo;
};

} // namespace sscl

#endif // SPINSCALE_SEQUENCE_LOCK_H
