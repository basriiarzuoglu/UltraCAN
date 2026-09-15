#pragma once

#include <QObject>
#include <QStringList>
#include <QUrl>
#include <QVector>
#include <QtQml/qqmlregistration.h>

#include <vector>

#include "core/CanFrame.h"

class BusReplayPacer;
class CanBusManager;
class QThread;

/*!
    \class LogReplayModel
    \brief Replays a recorded trace onto the PHYSICAL bus. Nothing else.

    \section1 Why this is a separate class and not a mode

    The first version of this feature was a \c transmitToBus flag on
    \c CanPlaybackModel, reached from the Graph Plot's transport bar. That was
    wrong, and the reason is worth writing down so it does not come back:

    \list
    \li \b {A flag can be left set.} The Graph Plot's transport is something an
        analyst drives dozens of times an hour while reading a trace. One
        control on that bar that sometimes means "energise the bus" is a
        control that will eventually be pressed while it means that.
    \li \b {Shared state has to be checked; separate objects cannot be wrong.}
        With one object and a flag, every path - play, seek, loop, a restored
        session - has to ask which mode it is in. With two objects the Graph
        Plot's instance has no pacer, no bus write path and no code that could
        acquire one. It is not that it is configured not to transmit; it
        cannot.
    \li \b {The two jobs want different clocks anyway.} Virtual replay is
        driven by a 16 ms tick because its consumers repaint at 60 Hz.
        Transmission is driven by \c BusReplayPacer on its own thread because
        its consumer is an ECU counting microseconds. Forcing both through one
        timeline was already costing more in special cases than it saved.
    \endlist

    So: \c CanPlaybackModel feeds the application and has no wire. This feeds
    the wire and has no application-side dispatch. They share \c TraceReader
    for parsing, and nothing else.

    \section1 Arming

    \l armed must be set before \l play() will do anything, and it is cleared
    by \l setArmed(false), by loading a different file, and by the bus going
    away. The Sender's Log Replay tab makes the user confirm once per session
    to set it. This is the software equivalent of a cover over a switch: the
    point is not that it is hard, it is that it cannot happen absent-mindedly.
*/
class LogReplayModel : public QObject
{
    Q_OBJECT
    QML_ANONYMOUS

    Q_PROPERTY(bool loaded READ isLoaded NOTIFY loadedChanged FINAL)
    Q_PROPERTY(QString sourceName READ sourceName NOTIFY loadedChanged FINAL)
    Q_PROPERTY(int count READ count NOTIFY loadedChanged FINAL)
    Q_PROPERTY(qreal duration READ duration NOTIFY loadedChanged FINAL)
    Q_PROPERTY(QStringList warnings READ warnings NOTIFY loadedChanged FINAL)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged FINAL)

    Q_PROPERTY(qreal position READ position NOTIFY positionChanged FINAL)
    Q_PROPERTY(bool playing READ isPlaying NOTIFY playingChanged FINAL)
    Q_PROPERTY(qreal speed READ speed WRITE setSpeed NOTIFY speedChanged FINAL)
    Q_PROPERTY(bool loop READ loop WRITE setLoop NOTIFY loopChanged FINAL)

    /*! The safety interlock. False until the user confirms, and \l play() is a
        no-op while it is false. */
    Q_PROPERTY(bool armed READ isArmed WRITE setArmed NOTIFY armedChanged FINAL)
    /*! True only while frames are actually leaving the adapter. */
    Q_PROPERTY(bool transmitting READ isTransmitting NOTIFY transmittingChanged FINAL)
    /*! False when there is no hardware to transmit to - the UI says so instead
        of letting the user arm a replay that would go nowhere. */
    Q_PROPERTY(bool busReady READ isBusReady NOTIFY busReadyChanged FINAL)
    Q_PROPERTY(qint64 framesSent READ framesSent NOTIFY progressChanged FINAL)

public:
    explicit LogReplayModel(QObject *parent = nullptr);
    ~LogReplayModel() override;

    /*! Borrowed. Also the source of \l busReady. */
    void setBusManager(CanBusManager *manager);
    /*! Names the CSV reader maps a channel column onto. */
    void setChannelNames(const QStringList &names);

    bool isLoaded() const { return !m_frames.empty(); }
    QString sourceName() const { return m_sourceName; }
    int count() const { return int(m_frames.size()); }
    qreal duration() const;
    QStringList warnings() const { return m_warnings; }
    QString lastError() const { return m_lastError; }

    qreal position() const;
    bool isPlaying() const { return m_playing; }
    qreal speed() const { return m_speed; }
    void setSpeed(qreal speed);
    bool loop() const { return m_loop; }
    void setLoop(bool loop);

    bool isArmed() const { return m_armed; }
    void setArmed(bool armed);
    bool isTransmitting() const { return m_transmitting; }
    bool isBusReady() const;
    qint64 framesSent() const { return m_framesSent; }

    Q_INVOKABLE bool loadLog(const QUrl &fileUrl);
    Q_INVOKABLE void clear();

    /*! Starts transmitting from the current position. Refused - with
        \l lastError set - when nothing is loaded, when the bus is not
        connected, or when \l armed is false. */
    Q_INVOKABLE void play();
    /*! Stops transmitting and keeps the position. */
    Q_INVOKABLE void pause();
    /*! Stops transmitting, rewinds, and DISARMS: ending a run should not leave
        a loaded transmitter one click away from running again. */
    Q_INVOKABLE void stop();
    /*! Moves the playhead. While transmitting, the run restarts from there. */
    Q_INVOKABLE void seek(qreal seconds);

    Q_INVOKABLE QString formatTime(qreal seconds) const;

signals:
    void loadedChanged();
    void lastErrorChanged();
    void positionChanged();
    void playingChanged();
    void speedChanged();
    void loopChanged();
    void armedChanged();
    void transmittingChanged();
    void busReadyChanged();
    void progressChanged();
    /*! One lap finished and the next started. */
    void looped();
    /*! The recording ran to its end without being stopped. */
    void completed();

    /*! Queued to the pacer thread. */
    void startPacer(const QVector<CanFrame> &frames, int startIndex, qreal speed, bool loop);

private:
    void ensurePacer();
    void setPlaying(bool playing);
    void setTransmitting(bool transmitting);
    void setLastError(const QString &message);
    void setPositionNs(qint64 positionNs);
    int indexAtPosition() const;

    CanBusManager *m_busManager = nullptr;
    QStringList m_channelNames;

    std::vector<CanFrame> m_frames;
    QString m_sourceName;
    QStringList m_warnings;
    QString m_lastError;
    qint64 m_originNs = 0;        //!< timestamp of the first frame
    qint64 m_durationNs = 0;
    qint64 m_positionNs = 0;

    bool m_playing = false;
    bool m_armed = false;
    bool m_transmitting = false;
    qreal m_speed = 1.0;
    bool m_loop = false;
    qint64 m_framesSent = 0;

    QThread *m_pacerThread = nullptr;
    BusReplayPacer *m_pacer = nullptr;
};
