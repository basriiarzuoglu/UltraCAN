#pragma once

#include <QString>
#include <QVector>

#include "core/CanFrame.h"
#include "dbc/CanDecoder.h"

/*!
    \struct TriggerCondition
    \brief One test applied to every frame on the bus: "has the thing I am
           waiting for happened yet?"

    \section1 Why the signal is held as a KEY, not a name

    A decoded signal is identified by an integer key into \c CanDecoder's flat
    table. Storing "EngineData.RPM" here instead would mean a string comparison
    per signal per frame on the acquisition thread - at 5 000 frames/s with
    twenty signals in the message, that is 100 000 QString compares a second to
    answer a question an \c int comparison already answers.

    The cost of that choice is that a key is only meaningful for as long as the
    decode table it indexes. \c CanBusManager::decoderRebuilt() says the table
    was replaced, and \c CanLogModel re-resolves the trigger from the names it
    kept alongside the key when that fires. A key that cannot be re-resolved
    goes back to -1, which never matches - a trigger pointing at a signal the
    current DBC does not have must not fire on whatever signal inherited its
    index.

    \section1 Evaluation is const and allocation-free

    \l matches() takes the caller's scratch vector rather than making one: it
    runs per frame on a thread whose only job is not to fall behind the bus.
*/
struct TriggerCondition
{
    enum Kind {
        None = 0,        //!< never fires
        IdMatch,         //!< a frame with this id appeared
        SignalValue      //!< a decoded signal crossed a threshold
    };

    enum Comparison {
        Greater = 0,
        GreaterEqual,
        Less,
        LessEqual,
        Equal,
        NotEqual
    };

    Kind kind = None;

    // -- IdMatch
    quint32 frameId = 0;
    bool extended = false;

    // -- SignalValue
    /*! Resolved index into the decode table, or -1 while unresolvable. */
    int signalKey = -1;
    /*! Kept so the key can be found again after a DBC reload. */
    QString messageName;
    QString signalName;
    Comparison comparison = Greater;
    double threshold = 0.0;

    bool isActive() const { return kind != None; }

    static bool compare(double value, Comparison how, double threshold)
    {
        switch (how) {
        case Greater:      return value >  threshold;
        case GreaterEqual: return value >= threshold;
        case Less:         return value <  threshold;
        case LessEqual:    return value <= threshold;
        // Physical values come out of a scale/offset multiplication, so an
        // exact == on a double is a test that a technician would watch fail
        // for no visible reason. A tolerance of one part in ten thousand of
        // the threshold (and 1e-9 near zero) is what "equal" has to mean here.
        case Equal:        return qAbs(value - threshold)
                                  <= qMax(1e-9, qAbs(threshold) * 1e-4);
        case NotEqual:     return qAbs(value - threshold)
                                  >  qMax(1e-9, qAbs(threshold) * 1e-4);
        }
        return false;
    }

    /*!
        True when \a frame satisfies this condition. \a decoder may be null and
        \a scratch is reused across calls.
    */
    bool matches(const CanFrame &frame, const CanDecoder *decoder,
                 QVector<DecodedSample> &scratch) const
    {
        switch (kind) {
        case None:
            return false;

        case IdMatch:
            return frame.id == frameId && frame.extended == extended;

        case SignalValue: {
            if (!decoder || signalKey < 0)
                return false;
            scratch.clear();
            // One frame, not a batch: the read lock is taken per call, but the
            // alternative - decoding the whole batch up front - would decode
            // every frame in it even though the condition usually rejects the
            // first one on its id.
            decoder->decode(frame, scratch);
            for (const DecodedSample &sample : scratch) {
                if (sample.key == signalKey)
                    return compare(sample.value, comparison, threshold);
            }
            return false;
        }
        }
        return false;
    }
};
