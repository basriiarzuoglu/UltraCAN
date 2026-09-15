#pragma once

#include <QObject>
#include <QElapsedTimer>
#include <QStringList>
#include <QUrl>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <vector>

#include "core/CanFrame.h"

class CanDecoder;
class QTimer;
class CanBusManager;

/*!
    \brief A recorded trace, and the clock that replays it.

    Storage is a contiguous `std::vector<CanFrame>` sorted by timestamp - the
    same POD the live path uses, so a 500k-frame log costs ~50 MB with no
    per-frame allocation and no pointer chasing while scrolling.

    \b {Scrubbing} is `std::lower_bound` over that vector: O(log n), so the
    scrubber tracks the mouse at 500k frames exactly as it does at 1k.

    \b {The clock} is deliberately NOT one timer per frame. Qt timers have
    millisecond granularity and a recorded bus routinely has 100 us gaps, so a
    per-frame timer would both drift and drown the event loop. Instead a 16 ms
    tick advances \l position by the WALL-CLOCK time elapsed since the last
    tick (QElapsedTimer, not the nominal interval, so a late tick does not slow
    playback down) multiplied by \l speed, and every frame whose timestamp
    falls in that window is reported through \l framesPlayed(). The timeline
    therefore honours the true inter-frame deltas even though the UI only
    refreshes 60 times a second.
*/
class CanPlaybackModel : public QObject
{
    Q_OBJECT
    // Registered by instance from main() - see the note there.
    QML_ANONYMOUS

    /*! Frames in the loaded recording. */
    Q_PROPERTY(int count READ count NOTIFY loadedChanged FINAL)
    /*! True from play() until stop(): the window during which this object owns
        the application's frame stream. Pause does NOT leave it - a paused log
        must not let live traffic back onto the same graph. */
    Q_PROPERTY(bool playbackActive READ isPlaybackActive NOTIFY playbackActiveChanged FINAL)
    Q_PROPERTY(bool loaded READ isLoaded NOTIFY loadedChanged FINAL)
    Q_PROPERTY(QString sourceName READ sourceName NOTIFY loadedChanged FINAL)
    Q_PROPERTY(qreal duration READ duration NOTIFY loadedChanged FINAL)
    Q_PROPERTY(QStringList warnings READ warnings NOTIFY loadedChanged FINAL)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged FINAL)

    // -- transport -------------------------------------------------------------
    Q_PROPERTY(qreal position READ position NOTIFY positionChanged FINAL)
    Q_PROPERTY(int currentIndex READ currentIndex NOTIFY currentIndexChanged FINAL)
    Q_PROPERTY(bool playing READ isPlaying NOTIFY playingChanged FINAL)
    Q_PROPERTY(qreal speed READ speed WRITE setSpeed NOTIFY speedChanged FINAL)
    Q_PROPERTY(bool loop READ loop WRITE setLoop NOTIFY loopChanged FINAL)


public:
    explicit CanPlaybackModel(QObject *parent = nullptr);
    ~CanPlaybackModel() override;

    static CanPlaybackModel *instance();

    /*! Wired in main(): used to name recorded frames from the loaded database. */
    void setDecoder(const CanDecoder *decoder);
    void setChannelNames(const QStringList &names);
    /*! Where replayed frames are injected - the same entry point the driver
        uses, which is what makes this a virtual bus rather than a private
        feed into one chart. */
    void setBusManager(CanBusManager *manager);

    bool isPlaybackActive() const { return m_playbackActive; }

    /*! Frames in the loaded log. */
    int count() const { return int(m_frames.size()); }

    // -- loaded recording ------------------------------------------------------
    // Trivial accessors, deliberately inline: every one of these is read by a
    // QML binding, so they are hit on each property notification.
    bool isLoaded() const { return !m_frames.empty(); }
    QString sourceName() const { return m_sourceName; }
    /*! Timestamp of the last frame, in seconds. */
    qreal duration() const { return double(m_durationNs) / double(1'000'000'000LL); }
    QStringList warnings() const { return m_warnings; }
    QString lastError() const { return m_lastError; }

    // -- transport -------------------------------------------------------------
    /*! Playhead position in seconds, on the RECORDING's clock (not wall time). */
    qreal position() const { return double(m_positionNs) / double(1'000'000'000LL); }
    /*! Index of the last frame at or before the playhead, -1 before the first. */
    int currentIndex() const { return m_currentIndex; }
    bool isPlaying() const { return m_playing; }
    qreal speed() const { return m_speed; }
    void setSpeed(qreal speed);
    bool loop() const { return m_loop; }
    void setLoop(bool loop);

    // -- file I/O --------------------------------------------------------------
    /*!
        Loads a recorded trace. Chooses the reader by extension and by sniffing
        the first lines: our own Logger CSV, PCAN .trc (1.x and 2.x) or Vector
        .asc. Returns false and sets \l lastError on failure.
    */
    Q_INVOKABLE bool loadLog(const QUrl &fileUrl);
    Q_INVOKABLE void clear();
    /*! Fabricated recording, for trying the player without a file. */
    Q_INVOKABLE void loadDemo(const QString &name, int frameCount);

    // -- transport (called from QML) -------------------------------------------
    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void seek(qreal seconds);
    Q_INVOKABLE void seekToIndex(int index);

    /*! Index of the last frame at or before \a seconds, -1 before the first. */
    Q_INVOKABLE int indexAtTime(qreal seconds) const;
    Q_INVOKABLE qreal timeAt(int index) const;
    Q_INVOKABLE QVariantMap frameAt(int index) const;
    /*! Average frames per second across the whole recording. */
    Q_INVOKABLE qreal averageRate() const;
    /*! Seconds -> "MM:SS.mmm". */
    Q_INVOKABLE QString formatTime(qreal seconds) const;

signals:
    void loadedChanged();
    void lastErrorChanged();
    void positionChanged();
    void currentIndexChanged();
    void playingChanged();
    void speedChanged();
    void loopChanged();
    /*!
        Frames [firstIndex, lastIndex] just crossed the playhead, in order.
        Hook for replaying a log onto the bus or through the decoder into the
        Graph Plot - the player itself only moves the clock.
    */
    void framesPlayed(int firstIndex, int lastIndex);
    void playbackActiveChanged();
    /*! The playhead JUMPED rather than advanced. Consumers holding a rolling
        window (the graph) must throw it away and restart from \a seconds. */
    void seeked(qreal seconds);

private:
    void tick();
    void setPlaying(bool playing);
    void setPositionNs(qint64 positionNs, bool dispatch);
    void setLastError(const QString &message);
    void setPlaybackActive(bool active);
    /*! Pushes frames (firstIndex..lastIndex] onto the virtual bus. */
    void dispatchRange(int firstIndex, int lastIndex);
    void applyFrames(std::vector<CanFrame> &&frames, const QString &name,
                     const QStringList &warnings);

    // Parsing lives in TraceReader - see the note there. This class is the
    // VIRTUAL replay: it feeds the application and has no path to the wire.

    static CanPlaybackModel *s_instance;

    const CanDecoder *m_decoder = nullptr;
    QStringList m_channelNames { QStringLiteral("CAN1"), QStringLiteral("CAN2") };

    std::vector<CanFrame> m_frames;      //!< sorted by timestampNs
    QString m_sourceName;
    QStringList m_warnings;
    QString m_lastError;
    qint64 m_durationNs = 0;

    QTimer *m_clock = nullptr;
    CanBusManager *m_busManager = nullptr;
    bool m_playbackActive = false;
    /*! Reused across ticks so a fast log does not allocate 60 times a second. */
    QVector<CanFrame> m_dispatchBuffer;
    QElapsedTimer m_wallClock;           //!< real time between ticks, not the nominal interval
    qint64 m_positionNs = 0;
    int m_currentIndex = -1;
    bool m_playing = false;
    bool m_loop = false;

    qreal m_speed = 1.0;
};
