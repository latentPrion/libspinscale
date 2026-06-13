#include <gtest/gtest.h>
#include <spinscale/cps/qutex.h>
#include <spinscale/cps/lockerAndInvokerBase.h>
#include <memory>
#include <stdexcept>
#include <thread>
#include <chrono>
#include <vector>

namespace smo {

// Mock implementation of LockerAndInvokerBase for testing
class MockLockerAndInvoker : public sscl::cps::LockerAndInvokerBase {
public:
    explicit MockLockerAndInvoker(const void* addr)
        : sscl::cps::LockerAndInvokerBase(addr), awakened(false) {}

    bool awakened;
    mutable sscl::cps::Qutex* registeredQutex = nullptr;
    mutable sscl::cps::LockerAndInvokerBase::List::iterator queueIterator;

    sscl::cps::LockerAndInvokerBase::List::iterator
    getLockvokerIteratorForQutex(sscl::cps::Qutex& qutex) const override
    {
        registeredQutex = &qutex;

        for (auto it = qutex.queue.begin(); it != qutex.queue.end(); ++it)
        {
            if ((**it) == *this)
            {
                queueIterator = it;
                return it;
            }
        }

        throw std::runtime_error(
            "MockLockerAndInvoker: not registered in qutex queue");
    }

    void awaken(bool forceAwaken = false) override
    {
        (void)forceAwaken;
        awakened = true;
    }

    size_t getLockSetSize() const override
    {
        return 1;
    }

    sscl::cps::Qutex& getLockAt(size_t index) const override
    {
        if (index != 0 || registeredQutex == nullptr)
        {
            throw std::runtime_error(
                "MockLockerAndInvoker: invalid lock index or no registered qutex");
        }

        return *registeredQutex;
    }
};

class QutexTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create mock lockvokers with unique addresses
        mock1 = std::make_shared<MockLockerAndInvoker>(&addr1);
        mock2 = std::make_shared<MockLockerAndInvoker>(&addr2);
        mock3 = std::make_shared<MockLockerAndInvoker>(&addr3);
        mock4 = std::make_shared<MockLockerAndInvoker>(&addr4);
        mock5 = std::make_shared<MockLockerAndInvoker>(&addr5);
    }

    void TearDown() override {
        // Clean up
    }

    sscl::cps::Qutex qutex{"test-qutex"};
    std::shared_ptr<MockLockerAndInvoker> mock1, mock2, mock3, mock4, mock5;

    // Unique addresses for testing
    int addr1 = 1;
    int addr2 = 2;
    int addr3 = 3;
    int addr4 = 4;
    int addr5 = 5;
};

// Test basic queue registration and unregistration
TEST_F(QutexTest, QueueRegistrationAndUnregistration) {
    // Register mock1 in queue
    auto it1 = qutex.registerInQueue(mock1);
    EXPECT_EQ(qutex.queue.size(), 1);
    EXPECT_FALSE(qutex.isOwned);

    // Register mock2 in queue
    auto it2 = qutex.registerInQueue(mock2);
    EXPECT_EQ(qutex.queue.size(), 2);

    // Unregister mock1
    qutex.unregisterFromQueue(it1);
    EXPECT_EQ(qutex.queue.size(), 1);

    // Unregister mock2
    qutex.unregisterFromQueue(it2);
    EXPECT_EQ(qutex.queue.size(), 0);
}

// Test single lock acquisition when queue is empty
TEST_F(QutexTest, SingleLockAcquisitionEmptyQueue) {
    // Register mock1
    (void)qutex.registerInQueue(mock1);

    // Try to acquire with nRequiredLocks = 1
    bool acquired = qutex.tryAcquire(*mock1, 1);
    EXPECT_TRUE(acquired);
    EXPECT_TRUE(qutex.isOwned);
}

// Test single lock acquisition when at front of queue
TEST_F(QutexTest, SingleLockAcquisitionAtFront) {
    // Register multiple lockvokers
    (void)qutex.registerInQueue(mock1);
    (void)qutex.registerInQueue(mock2);
    (void)qutex.registerInQueue(mock3);

    // mock1 should be at front, mock3 at back
    EXPECT_EQ(qutex.queue.front().get(), mock1.get());
    EXPECT_EQ(qutex.queue.back().get(), mock3.get());

    // mock1 (at front) should succeed
    bool acquired = qutex.tryAcquire(*mock1, 1);
    EXPECT_TRUE(acquired);
    EXPECT_TRUE(qutex.isOwned);

    // mock2 (not at front) should fail
    qutex.isOwned = false; // Reset for testing
    bool acquired2 = qutex.tryAcquire(*mock2, 1);
    EXPECT_FALSE(acquired2);
}

// Test single lock acquisition failure when not at front
TEST_F(QutexTest, SingleLockAcquisitionNotAtFront) {
    // Register multiple lockvokers
    (void)qutex.registerInQueue(mock1);
    (void)qutex.registerInQueue(mock2);

    // mock2 (not at front) should fail
    bool acquired = qutex.tryAcquire(*mock2, 1);
    EXPECT_FALSE(acquired);
    EXPECT_FALSE(qutex.isOwned);
}

// Test multi-lock acquisition (nRequiredLocks > 1)
TEST_F(QutexTest, MultiLockAcquisition) {
    // Register 4 lockvokers
    (void)qutex.registerInQueue(mock1);
    (void)qutex.registerInQueue(mock2);
    (void)qutex.registerInQueue(mock3);
    (void)qutex.registerInQueue(mock4);

    // For nRequiredLocks = 2, need to be in top 50% (top 2 out of 4)
    // mock1 (position 1) should succeed
    bool acquired1 = qutex.tryAcquire(*mock1, 2);
    EXPECT_TRUE(acquired1);

    // Reset for next test
    qutex.isOwned = false;

    // mock2 (position 2) should succeed
    bool acquired2 = qutex.tryAcquire(*mock2, 2);
    EXPECT_TRUE(acquired2);

    // Reset for next test
    qutex.isOwned = false;

    // mock3 (position 3) should fail (in bottom 50%)
    bool acquired3 = qutex.tryAcquire(*mock3, 2);
    EXPECT_FALSE(acquired3);

    // Reset for next test
    qutex.isOwned = false;

    // mock4 (position 4) should fail (in bottom 50%)
    bool acquired4 = qutex.tryAcquire(*mock4, 2);
    EXPECT_FALSE(acquired4);
}

// Test multi-lock acquisition with 3 required locks
TEST_F(QutexTest, MultiLockAcquisitionThreeLocks) {
    // Register 6 lockvokers
    (void)qutex.registerInQueue(mock1);
    (void)qutex.registerInQueue(mock2);
    (void)qutex.registerInQueue(mock3);
    (void)qutex.registerInQueue(mock4);
    (void)qutex.registerInQueue(mock5);

    // Create one more mock
    int addr6 = 6;
    auto mock6 = std::make_shared<MockLockerAndInvoker>(&addr6);
    (void)qutex.registerInQueue(mock6);

    // For nRequiredLocks = 3, need to be in top 66% (top 4 out of 6)
    // Positions 1, 2, 3, 4 should succeed
    // Positions 5, 6 should fail

    bool acquired1 = qutex.tryAcquire(*mock1, 3);
    EXPECT_TRUE(acquired1);
    qutex.isOwned = false;

    bool acquired2 = qutex.tryAcquire(*mock2, 3);
    EXPECT_TRUE(acquired2);
    qutex.isOwned = false;

    bool acquired3 = qutex.tryAcquire(*mock3, 3);
    EXPECT_TRUE(acquired3);
    qutex.isOwned = false;

    bool acquired4 = qutex.tryAcquire(*mock4, 3);
    EXPECT_TRUE(acquired4);
    qutex.isOwned = false;

    bool acquired5 = qutex.tryAcquire(*mock5, 3);
    EXPECT_FALSE(acquired5);

    qutex.isOwned = false;
    bool acquired6 = qutex.tryAcquire(*mock6, 3);
    EXPECT_FALSE(acquired6);
}

// Test acquisition failure when already owned
TEST_F(QutexTest, AcquisitionFailureWhenOwned) {
    // Register mock1
    (void)qutex.registerInQueue(mock1);

    // Manually set as owned
    qutex.isOwned = true;

    // Try to acquire should fail
    bool acquired = qutex.tryAcquire(*mock1, 1);
    EXPECT_FALSE(acquired);
    EXPECT_TRUE(qutex.isOwned);
}

// Test backoff with single item (should not rotate)
TEST_F(QutexTest, BackoffSingleItem) {
	// Register only one lockvoker
	(void)qutex.registerInQueue(mock1);

	// Set as owned first
	qutex.isOwned = true;

	// nRequiredLocks > 1 avoids the "front item with nRequiredLocks==1" guard
	mock1->awakened = false;
	qutex.backoff(*mock1, 2);

	EXPECT_FALSE(qutex.isOwned);
	EXPECT_EQ(qutex.queue.size(), 1u);
	// Should not awaken since there's only one item
	EXPECT_FALSE(mock1->awakened);
}

// Test backoff with multiple items and rotation
TEST_F(QutexTest, BackoffWithRotation) {
    // Register multiple lockvokers
    (void)qutex.registerInQueue(mock1);
    (void)qutex.registerInQueue(mock2);
    (void)qutex.registerInQueue(mock3);

    // Set as owned first
    qutex.isOwned = true;

    // mock1 should be at front initially
    EXPECT_EQ(qutex.queue.front().get(), mock1.get());

    // Backoff from mock1 (at front) with nRequiredLocks = 2
    mock2->awakened = false;
    qutex.backoff(*mock1, 2);

    // mock1 should have been rotated to position 2
    // mock2 should now be at front
    EXPECT_EQ(qutex.queue.front().get(), mock2.get());
    EXPECT_FALSE(qutex.isOwned);

    // mock2 should have been awakened
    EXPECT_TRUE(mock2->awakened);
}

// Test backoff with rotation to back when queue smaller than nRequiredLocks
TEST_F(QutexTest, BackoffRotationToBack) {
    // Register only 2 lockvokers
    (void)qutex.registerInQueue(mock1);
    (void)qutex.registerInQueue(mock2);

    // Set as owned first
    qutex.isOwned = true;

    // mock1 should be at front initially
    EXPECT_EQ(qutex.queue.front().get(), mock1.get());
    EXPECT_EQ(qutex.queue.back().get(), mock2.get());

    // Backoff from mock1 with nRequiredLocks = 5 (larger than queue size)
    mock2->awakened = false;
    qutex.backoff(*mock1, 5);

    // mock1 should have been moved to the back
    EXPECT_EQ(qutex.queue.front().get(), mock2.get());
    EXPECT_EQ(qutex.queue.back().get(), mock1.get());
    EXPECT_FALSE(qutex.isOwned);

    // mock2 should have been awakened
    EXPECT_TRUE(mock2->awakened);
}

// Test release functionality
TEST_F(QutexTest, Release) {
	// Register multiple lockvokers
	(void)qutex.registerInQueue(mock1);
	(void)qutex.registerInQueue(mock2);

	ASSERT_TRUE(qutex.tryAcquire(*mock1, 1));

	// Release should set isOwned to false and awaken front item
	mock1->awakened = false;
	qutex.release();

	EXPECT_FALSE(qutex.isOwned);
	EXPECT_TRUE(mock1->awakened);
}

// Test release without a prior acquire is rejected
TEST_F(QutexTest, ReleaseWithoutAcquireThrows) {
	qutex.isOwned = true;

	EXPECT_THROW(qutex.release(), std::runtime_error);
	EXPECT_TRUE(qutex.queue.empty());
}

// Test exception when trying to acquire from empty queue
TEST_F(QutexTest, ExceptionOnEmptyQueueAcquisition) {
    // Don't register any lockvokers
    EXPECT_THROW(qutex.tryAcquire(*mock1, 1), std::runtime_error);
}

// Test exception when backoff called on empty queue
TEST_F(QutexTest, ExceptionOnEmptyQueueBackoff) {
    // Don't register any lockvokers
    EXPECT_THROW(qutex.backoff(*mock1, 1), std::runtime_error);
}

// Test edge case: single lockvoker with multiple required locks
TEST_F(QutexTest, SingleLockvokerMultipleRequiredLocks) {
    // Register only one lockvoker
    (void)qutex.registerInQueue(mock1);

    // Should succeed regardless of nRequiredLocks when only one item
    bool acquired = qutex.tryAcquire(*mock1, 5);
    EXPECT_TRUE(acquired);
    EXPECT_TRUE(qutex.isOwned);
}

// Test unregistration without locking
TEST_F(QutexTest, UnregistrationWithoutLocking) {
    // Register lockvoker
    auto it1 = qutex.registerInQueue(mock1);
    EXPECT_EQ(qutex.queue.size(), 1);

    // Unregister without locking
    qutex.unregisterFromQueue(it1, false);
    EXPECT_EQ(qutex.queue.size(), 0);
}

} // namespace smo
