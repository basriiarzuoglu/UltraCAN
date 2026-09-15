#include "BusReplayPacer.h"

#include "core/CanBusManager.h"

#include <chrono>
#include <thread>

namespace {
constexpr qint64 NsPerUs = 1000;
}

BusReplayPacer::BusReplayPacer(QObject *parent)
    : QObject(parent)
{
}

void BusReplayPacer::setBusManager(CanBusManager *manager)
{
    m_bus = manager;
}

qint64 BusReplayPacer::steadyNowNs()
{
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

void BusReplayPacer::setSpeed(qreal speed)
{
    // Clamped here as well as in the model: this is the value the timing maths
    // divides by, and a zero would be an infinite deadline rather than a pause.
    const int milli = int(qBound(0.01, double(speed), 100.0) * 1000.0);
    m_speedMilli.store(milli, std::memory_order_relaxed);
    // Wake the sleeper: a speed change during a long gap should shorten that
    // gap, not take effect after it.
    m_sleepCv.notify_all();
}

void BusReplayPacer::setLoop(bool loop)
{
    m_loop.store(loop, std::memory_order_relaxed);
}

void BusReplayPacer::stop()
{
    m_stopRequested.store(true, std::memory_order_relaxed);
    m_sleepCv.notify_all();
}

bool BusReplayPacer::waitUntil(qint64 deadlineNs)
{
    // -- stage one: give the core back until we are close.
    for (;;) {
        if (m_stopRequested.load(std::memory_order_relaxed))
            return false;

        const qint64 remaining = deadlineNs - steadyNowNs();
        if (remaining <= SpinThresholdUs * NsPerUs)
            break;

        std::unique_lock<std::mutex> lock(m_sleepMutex);
        // wait_for, not sleep_for: stop() and setSpeed() notify, so this
        // returns early instead of holding the transport hostage to a gap.
        m_sleepCv.wait_for(lock, std::chrono::nanoseconds(remaining - SpinThresholdUs * NsPerUs));
        // Loop back and re-measure rather than trusting the wait's own timing:
        // a spurious wake, a speed change, and an on-time return are the same
        // code path if the only thing believed is the clock.
    }

    // -- stage two: spin. Only ever SpinThresholdUs long, and this is what
    // makes a 100 us inter-frame gap reproducible on an OS whose sleep
    // granularity is ten times that.
    while (steadyNowNs() < deadlineNs) {
        if (m_stopRequested.load(std::memory_order_relaxed))
            return false;
        std::this_thread::yield();
    }
    return true;
}

void BusReplayPacer::runFrom(const QVector<CanFrame> &frames, int startIndex,
                             qreal speed, bool loop)
{
    if (frames.isEmpty() || !m_bus) {
        emit finished(true);
        return;
    }

    m_stopRequested.store(false, std::memory_order_relaxed);
    m_running.store(true, std::memory_order_relaxed);
    setSpeed(speed);
    m_loop.store(loop, std::memory_order_relaxed);

    const qint64 traceStartNs = frames.first().timestampNs;
    int index = qBound(0, startIndex, frames.size() - 1);
    bool completed = true;

    for (;;) {
        // Re-anchored per lap and after every speed change. Every deadline in
        // the run below is derived from THIS pair, so a frame that goes out
        // late costs only itself - the error cannot accumulate across a long
        // recording the way sleeping for each delta in turn would.
        qint64 originNs = steadyNowNs();
        qint64 anchorTraceNs = frames.at(index).timestampNs;
        int speedAtAnchor = m_speedMilli.load(std::memory_order_relaxed);
        qint64 lastProgressNs = 0;

        for (; index < frames.size(); ++index) {
            const int currentSpeed = m_speedMilli.load(std::memory_order_relaxed);
            if (currentSpeed != speedAtAnchor) {
                // Re-anchor instead of rescaling the remaining deadlines: the
                // frames already sent happened when they happened, and the new
                // rate applies from here forward.
                originNs = steadyNowNs();
                anchorTraceNs = frames.at(index).timestampNs;
                speedAtAnchor = currentSpeed;
            }

            const qint64 offsetNs = qint64(double(frames.at(index).timestampNs - anchorTraceNs)
                                           * 1000.0 / double(speedAtAnchor));
            if (!waitUntil(originNs + offsetNs)) {
                completed = false;
                break;
            }

            m_bus->sendFrame(frames.at(index));

            const qint64 nowNs = steadyNowNs();
            if (nowNs - lastProgressNs >= ProgressIntervalUs * NsPerUs) {
                lastProgressNs = nowNs;
                emit progress(frames.at(index).timestampNs - traceStartNs);
            }
        }

        if (!completed)
            break;

        emit progress(frames.last().timestampNs - traceStartNs);

        if (!m_loop.load(std::memory_order_relaxed))
            break;

        index = 0;
        emit looped();
    }

    m_running.store(false, std::memory_order_relaxed);
    emit finished(completed);
}
