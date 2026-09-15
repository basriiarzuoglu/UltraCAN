#pragma once

#include "core/CanFrame.h"

/*!
    \struct LogFilter
    \brief Decides which frames the background log is allowed to see at all.

    \section1 Filters and triggers are different questions

    A trigger answers \e when: the file waits, then records. A filter answers
    \e what: an id outside it never reaches the file, never reaches the
    pre-trigger ring, and - this is the part worth being deliberate about -
    never gets to fire a trigger either.

    That ordering is a decision, not an accident. A filter set to "only 0x100"
    and a start trigger on 0x200 means the capture can never begin, which reads
    as a bug until you notice the two controls contradict each other. The
    alternative - letting a filtered-out frame fire the trigger - would produce
    a file whose first frame is one the user asked never to record. Between a
    contradiction that does nothing and a contradiction that writes something
    incoherent, the first is the one you can debug, and the dialog warns about
    it in the moment rather than leaving it to be discovered.

    \section1 Cost

    \l allows() runs on every frame of a live bus, so it is a switch over an
    enum and at most two integer compares. No allocation, no decode, no lock:
    the payload test reads one byte out of the frame's inline array.
*/
struct LogFilter
{
    enum Mode {
        PassAll = 0,     //!< no filtering; the default
        SingleId,        //!< exactly one id
        IdRange,         //!< startId..endId inclusive
        PayloadMask      //!< one id, and one masked byte within it
    };

    Mode mode = PassAll;
    quint32 startId = 0;      //!< the id for SingleId/PayloadMask, the low end for IdRange
    quint32 endId = 0;        //!< IdRange only
    int byteIndex = 0;        //!< PayloadMask only, 0..7
    quint8 payloadMask = 0xFF;
    quint8 payloadMatch = 0;

    bool isActive() const { return mode != PassAll; }

    bool allows(const CanFrame &frame) const
    {
        switch (mode) {
        case PassAll:
            return true;

        case SingleId:
            return frame.id == startId;

        case IdRange:
            // Tolerated reversed rather than refused: a user who types the
            // range the other way round means the range, and silently logging
            // nothing would be a worse answer than logging what they meant.
            return startId <= endId ? (frame.id >= startId && frame.id <= endId)
                                    : (frame.id >= endId && frame.id <= startId);

        case PayloadMask: {
            if (frame.id != startId)
                return false;
            // A byte the frame does not carry cannot match. A short frame here
            // is normal traffic, not an error - the same id can be sent with a
            // shorter DLC - so it is blocked quietly rather than reported.
            if (byteIndex < 0 || byteIndex >= int(frame.dlc)
                || byteIndex >= int(frame.payload.size()))
                return false;
            return quint8(frame.payload[size_t(byteIndex)] & payloadMask) == payloadMatch;
        }
        }
        return true;
    }
};
