#pragma once

#include <QObject>
#include <QVector>

#include <atomic>
#include <condition_variable>
#include <mutex>

#include "core/CanFrame.h"

class CanBusManager;

/*!
    \class BusReplayPacer
    \brief Transmits a recorded trace back onto the wire with the original
           inter-frame timing.

    \section1 Why this exists when CanPlaybackModel already replays

    \c CanPlaybackModel drives a 16 ms tick and hands every frame that fell in
    the window to the application's own bus - the graph, the analyzer, the
    logger. For that job the tick is exactly right: nothing downstream can tell
    the difference, because none of it is looking at sub-frame timing, and one
    batch per repaint is far cheaper than one timer per frame.

    Putting those same batches on a REAL bus is a different proposition. A
    recorded ECU sends its 10 ms message every 10 ms; a 16 ms tick would emit
    two of them at once and then nothing for 32 ms. Any receiver that watches
    cycle times - which is most of them, and all of the ones worth simulating
    for - sees a bus that is visibly broken. Restbus simulation is the one
    consumer for which the inter-frame delta IS the payload.

    So this class exists for the timing alone. It does not parse, does not own
    the trace, and does not decide what to play: \c CanPlaybackModel remains
    the model, and hands this a const view of its frames.

    \section1 The clock

    \c std::chrono::steady_clock, anchored ONCE at the start of a run, with
    each frame due at \c {origin + (frame.timestamp - firstFrame.timestamp) /
    speed}. Deriving every deadline from the same origin rather than sleeping
    for each delta in turn is what keeps the error from accumulating: a sleep
    that overshoots by 200 us costs that frame 200 us and costs the frames
    after it nothing.

    \section1 Sleeping accurately enough

    A condition-variable wait is subject to the OS scheduler - about 1 ms of
    granularity on Windows, which is the platform this ships on and is coarser
    than the gaps being reproduced. So the wait is in two parts: sleep on the
    condition variable until \c SpinThresholdUs before the deadline, then spin
    with \c yield() for the remainder. The spin costs one core for at most a
    quarter of a millisecond per frame and is the only way to hit a 100 us gap
    without asking the user to change their system timer resolution.

    The condition variable is not just a sleep: \l stop() notifies it, so a
    thread parked on a 5-second gap in a sparse recording ends immediately
    rather than when the gap expires.

    \section1 Threading

    Lives on its own QThread. \l runFrom() blocks that thread for the length of
    the replay, so nothing else may be queued to this object while it runs -
    control goes through the atomics (\l stop(), \l setSpeed(), \l setLoop())
    which are readable mid-loop.

    Transmission calls \c CanBusManager::sendFrame(), which posts to the device
    thread and echoes into the trace. That is already how the device thread
    reaches it, so calling it from here adds no new cross-thread path.
*/
class BusReplayPacer : public QObject
{
    Q_OBJECT

public:
    explicit BusReplayPacer(QObject *parent = nullptr);

    /*! How long before a deadline to stop sleeping and start spinning. */
    static constexpr int SpinThresholdUs = 250;
    /*! How often progress reaches the UI - 30 Hz, not per frame: a 5 000 fps
        recording would otherwise post 5 000 queued signals a second at a GUI
        thread that can act on 60. */
    static constexpr int ProgressIntervalUs = 33000;

    void setBusManager(CanBusManager *manager);

    bool isRunning() const { return m_running.load(std::memory_order_relaxed); }

public slots:
    /*!
        Transmits \a frames from index \a startIndex to the end, then loops or
        finishes. Blocks this object's thread until it is done or \l stop() is
        called.

        \a frames is taken by value: it is a copy of the model's vector made on
        the GUI thread before the run starts, so the model stays free to load a
        different file without tearing the wire out from under a live replay.
    */
    void runFrom(const QVector<CanFrame> &frames, int startIndex, qreal speed, bool loop);
    /*! Ends the run. Safe from any thread, returns immediately - the pacer
        thread wakes out of its sleep and unwinds. */
    void stop();
    void setSpeed(qreal speed);
    void setLoop(bool loop);

signals:
    /*! Position within the recording, in nanoseconds, at ~30 Hz. */
    void progress(qint64 positionNs);
    /*! \a completed is false when \l stop() ended the run. */
    void finished(bool completed);
    /*! One lap ended and the next began - the model rebases its consumers. */
    void looped();

private:
    /*! Sleeps until \a deadlineNs measured on the steady clock, in two stages.
        Returns false if the run was stopped while waiting. */
    bool waitUntil(qint64 deadlineNs);
    static qint64 steadyNowNs();

    CanBusManager *m_bus = nullptr;

    std::atomic_bool m_running { false };
    std::atomic_bool m_stopRequested { false };
    std::atomic_bool m_loop { false };
    /*! Stored as an integer of thousandths so it is lock-free on every
        platform - std::atomic<double> is only guaranteed lock-free since C++20
        and not on every target even then. */
    std::atomic_int m_speedMilli { 1000 };

    std::mutex m_sleepMutex;
    std::condition_variable m_sleepCv;
};
