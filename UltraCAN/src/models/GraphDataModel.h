#pragma once

#include <QHash>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QPointF>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>
#include <QtQml/qqmlregistration.h>

#include <atomic>
#include <memory>
#include <vector>

#include "core/CanFrame.h"

class MathChannelManager;
#include "dbc/CanDecoder.h"
#include "dbc/DbcParser.h"

class QTimer;
class QXYSeries;

/*!
    \brief Owns the time-series buffers behind the Graph Plot, its own set of
           databases, and the fan-out to any number of chart panes.

    \section1 Three rules

    \b {Never append from QML.} \c LineSeries.append() from a JavaScript timer
    emits a change signal per point and re-lays-out the series each time; at a
    few hundred samples a second the UI stops being interactive. The points live
    in C++ QLists and are handed over with \c QXYSeries::replace(QList<QPointF>)
    - one call, one relayout, per series per frame.

    \b {Decouple acquisition from painting.} \l ingestFrames() runs on the CAN
    device thread and only appends to a mutex-protected staging vector. A 60 Hz
    timer on the GUI thread drains it, trims each buffer to the visible window
    and calls replace(). A 5 kHz bus still repaints sixty times a second, and
    memory is bounded by the window, not by uptime.

    \b {A track is a SERIES, not a signal.} This is what makes multiple panes
    work. The old model keyed tracks by decoder key, so asking for a signal that
    was already plotted returned "already tracked" and silently did nothing -
    the second pane got an empty chart. Now one signal can feed as many series
    as there are panes, and \c m_tracksByDbcKey maps one decoded value onto all
    of them.

    \section1 Its own databases

    Like the Raw Data Analyzer, the Graph Plot resolves against \l loadedDbcs -
    a private list, managed from its own panel - and not against whatever the
    DBC Editor happens to have open. Each file gets its own CanDecoder, so two
    databases with overlapping ids never share a lookup table.

    That is also why this class takes raw frames rather than DecodedSamples: the
    samples the bus manager used to hand over carried keys from ITS decoder, and
    those mean nothing here.
*/
class GraphDataModel : public QObject
{
    Q_OBJECT
    // NOT QML_ELEMENT / QML_SINGLETON: this instance is created and owned by
    // main() and published with qmlRegisterSingletonInstance(). Letting the
    // engine register the type as well would give QML a SECOND, unwired
    // instance - which is exactly what happened before.
    QML_ANONYMOUS

    Q_PROPERTY(qreal windowSeconds READ windowSeconds WRITE setWindowSeconds
               NOTIFY windowSecondsChanged FINAL)
    Q_PROPERTY(bool paused READ isPaused WRITE setPaused NOTIFY pausedChanged FINAL)
    /*! Newest sample time in seconds; refreshed at repaint rate - bind the X axis to it. */
    Q_PROPERTY(qreal latestTime READ latestTime NOTIFY refreshed FINAL)
    Q_PROPERTY(int trackedCount READ trackedCount NOTIFY tracksChanged FINAL)
    Q_PROPERTY(int pointCount READ pointCount NOTIFY refreshed FINAL)
    /*! Samples per second across all tracked signals, updated once a second. */
    Q_PROPERTY(int sampleRate READ sampleRate NOTIFY sampleRateChanged FINAL)
    /*! Increments once per repaint. Bind readouts to it - see the note in
        CanAnalyzerModel for why an invokable needs a ticking dependency. */
    Q_PROPERTY(int refreshTick READ refreshTick NOTIFY refreshed FINAL)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged FINAL)

    // -- the graph's OWN databases -------------------------------------------
    Q_PROPERTY(QVariantList loadedDbcs READ loadedDbcs NOTIFY databasesChanged FINAL)
    Q_PROPERTY(int databaseCount READ databaseCount NOTIFY databasesChanged FINAL)
    /*! Total signals across every loaded database - 0 means nothing can plot. */
    Q_PROPERTY(int availableSignalCount READ availableSignalCount NOTIFY databasesChanged FINAL)

public:
    explicit GraphDataModel(QObject *parent = nullptr);
    ~GraphDataModel() override;

    static GraphDataModel *instance();

    qreal windowSeconds() const { return m_windowSeconds; }
    void setWindowSeconds(qreal seconds);
    bool isPaused() const { return m_paused.load(std::memory_order_relaxed); }
    void setPaused(bool paused);
    qreal latestTime() const { return m_latestTime; }
    int trackedCount() const { return int(m_tracks.size()); }
    int pointCount() const { return m_pointCount; }
    int sampleRate() const { return m_sampleRate; }
    int refreshTick() const { return m_refreshTick; }
    QString lastError() const { return m_lastError; }

    // -- database management ---------------------------------------------------
    QVariantList loadedDbcs() const;
    int databaseCount() const { return int(m_databases.size()); }
    int availableSignalCount() const;

    Q_INVOKABLE bool loadGraphDbc(const QUrl &url);
    Q_INVOKABLE void removeGraphDbc(int dbcIndex);
    Q_INVOKABLE void clearGraphDbcs();

    /*!
        The whole tree the pane menus are built from:
        \c [{ dbcIndex, dbcName, messages: [{ messageIndex, name, idText,
             signals: [{ signalIndex, name, unit }] }] }]

        Rebuilt only when the database list changes - never bind it to anything
        that ticks at the repaint rate.
    */
    Q_INVOKABLE QVariantList signalCatalog() const;
    /*!
        Flat, filtered, capped. A production database has hundreds of signals,
        which is unusable as a cascading menu and fine as a search box.
        Each entry: { dbcIndex, messageIndex, signalIndex, dbcName, messageName,
        signalName, unit, label }.
    */
    Q_INVOKABLE QVariantList searchSignals(const QString &needle, int limit = 200) const;

    /*!
        Resolves a signal by NAME into the { dbcIndex, messageIndex, signalIndex }
        triple trackSignal() takes. Returns \c { found: false } when nothing
        matches. \a dbcName may be empty, which searches every loaded database
        and takes the first hit.

        Why names, when tracksForPane() already hands out indices: those indices
        are positions in \c m_databases and in the parsed tree, so they are only
        meaningful for as long as the exact same files are loaded in the exact
        same order. A saved session is read back on another machine, after the
        user has added a fourth .dbc, or with the files listed in a different
        order - and index 2 is then a different signal, silently. Names survive
        all three. The cost is one linear scan per signal at import time, which
        happens once.
    */
    Q_INVOKABLE QVariantMap findSignal(const QString &dbcName,
                                       const QString &messageName,
                                       const QString &signalName) const;

    // -- tracking, per pane ----------------------------------------------------
    /*!
        Feeds \a series (from ChartView.createSeries()) with one signal, on
        behalf of \a paneId. Returns the track id, or -1 with \l lastError set.

        The same signal may be tracked by several panes at once; each gets its
        own track and its own point buffer.
    */
    Q_INVOKABLE int trackSignal(int dbcIndex, int messageIndex, int signalIndex,
                                QObject *series, const QString &paneId);

    /*!
        Plots a math channel on \a series, exactly as \l trackSignal() plots a
        decoded one. Returns the track id, or -1 with \l lastError set.

        The pane does not need to know which kind it asked for: both produce a
        track with a name, a unit and a colour, and both are removed by
        \l untrack() and counted by \l trackedCount().
    */
    Q_INVOKABLE int trackMathChannel(int channelId, QObject *series, const QString &paneId);

    /*! Math channels available to plot, for the picker:
        [{ id, name, unit, expression, valid, error, bindings }]. */
    Q_INVOKABLE QVariantList mathChannels() const;

    /*! Wired in main(). Without one, math channels simply do not exist and
        every other path is unaffected. */
    void setMathChannelManager(MathChannelManager *manager);
    /*! Drops one track by the id trackSignal() returned. */
    Q_INVOKABLE void untrack(int trackId);
    /*! Drops every track belonging to \a paneId - what a pane calls when it closes. */
    Q_INVOKABLE void untrackPane(const QString &paneId);
    Q_INVOKABLE void untrackAll();
    Q_INVOKABLE void clear();
    /*!
        Empties the traces and moves the time origin to \a seconds.

        \b {Why this is needed at all.} This model already plots on the
        timestamp each FRAME carries - there is no QElapsedTimer here and never
        was, so a replayed log already lands on the log's own time. The problem
        is that \c m_latestTime only ever climbs: `if (seconds > m_latestTime)`.
        Scrub backwards, or switch from a bus sitting at t=3000 s to a log that
        starts at t=0, and the X window stays pinned where it was while every
        new point falls outside it and is trimmed on arrival. The graph looks
        broken and the data is fine.

        So the jump has to be explicit. Callers: a scrub, and the Live/Playback
        switch.
    */
    Q_INVOKABLE void rebaseTo(qreal seconds);

    /*! What \a paneId is plotting: [{ trackId, name, unit, minimum, maximum,
        colorIndex, dbcName, messageName, lastValue }]. */
    Q_INVOKABLE QVariantList tracksForPane(const QString &paneId) const;
    Q_INVOKABLE double lastValue(int trackId) const;
    Q_INVOKABLE QString lastValueText(int trackId) const;
    /*! Y-axis hints from the DATABASE's declared range, over one pane's tracks
        only. Used as the starting point before any samples arrive. */
    Q_INVOKABLE qreal paneMinimum(const QString &paneId) const;
    Q_INVOKABLE qreal paneMaximum(const QString &paneId) const;
    /*!
        Y-axis extent of the data ACTUALLY on screen for one pane.

        This is what auto-scale uses, and it is why a raw counter whose .dbc
        declares a maximum of 2^64-1 no longer flattens its own trace against
        the bottom of the pane. Returns false when the pane has no points yet,
        in which case the caller should fall back to the declared range.
    */
    Q_INVOKABLE bool paneDataRange(const QString &paneId, QVariantMap *out = nullptr) const;
    Q_INVOKABLE qreal paneDataMinimum(const QString &paneId) const;
    Q_INVOKABLE qreal paneDataMaximum(const QString &paneId) const;
    Q_INVOKABLE bool paneHasData(const QString &paneId) const;

    // -- called from the acquisition thread ------------------------------------
    /*!
        Thread-safe. Decodes with THIS model's databases and keeps only what is
        plotted - a frame nobody is watching costs one hash lookup.
    */
    void ingestFrames(const QVector<CanFrame> &frames);
    /*! True when at least one signal is tracked; lets the bus manager skip us. */
    bool isActive() const { return m_active.load(std::memory_order_relaxed); }

signals:
    void windowSecondsChanged();
    void pausedChanged();
    void tracksChanged();
    void refreshed();
    void sampleRateChanged();
    void lastErrorChanged();
    void databasesChanged();
    /*! The set of plottable math channels changed - the picker rebuilds. */
    void mathChannelsChanged();
    /*! Tracking started or stopped entirely, so the bus manager can skip the
        call altogether while nothing is plotted. */
    void activeChanged(bool active);

private:
    /*! One loaded file. Heap-held decoder: CanDecoder owns a QReadWriteLock so
        it cannot be copied, and tracks hold a raw pointer that must survive the
        vector growing. */
    struct GraphDatabase {
        QString name;
        QString path;
        QUrl url;
        int messageCount = 0;
        int signalCount = 0;
        /*! Kept alongside the decoder because CanDecoder compiles the database
            into flat lookup tables and does not retain the tree - and the
            catalog the pane menus are built from needs the tree. */
        DbcDatabase parsed;
        std::unique_ptr<CanDecoder> decoder;
    };

    /*! One series being fed. Keyed by nothing - identified by \c id. */
    struct Track {
        int id = -1;
        /*! >= 0 for a MATH channel. A virtual track has no dbcIndex, no
            decoder key and no frame to watch; it is fed by evaluate-on-refresh
            instead of by the decode fan-out, and rebuildIngestIndex() must
            skip it or it would register a watch on frame id 0. */
        int mathChannelId = -1;
        int dbcIndex = -1;
        int key = -1;                 //!< decoder key WITHIN that database
        quint32 frameKey = 0;         //!< id | extended<<31, for the ingest filter
        int messageIndex = -1;
        int signalIndex = -1;
        QString paneId;
        QString signalName;
        QString messageName;
        QString dbcName;
        QString unit;
        double minimum = 0.0;
        double maximum = 0.0;
        int colorIndex = 0;
        QXYSeries *series = nullptr;
        QList<QPointF> points;
        double lastValue = 0.0;
        // Extent of `points`, recomputed at readout rate rather than every
        // repaint: a full min/max over every point of every track at 60 Hz is
        // real work for an axis nobody can see move that fast.
        double dataMinimum = 0.0;
        double dataMaximum = 0.0;
        bool hasData = false;
    };

    /*! Resolved on the producer thread, so refresh() is a pure copy. */
    struct PendingSample {
        int trackIndex = -1;
        qint64 timestampNs = 0;
        double value = 0.0;
    };

    void setLastError(const QString &message);
    void refresh();                       //!< GUI thread, 60 Hz
    /*! Evaluates every math track whose inputs moved and appends one point
        each. GUI thread, called from refresh() - see MathChannelManager. */
    void evaluateMathTracks();
    void updateActive();
    void removeTrackAt(int index);
    /*!
        Rebuilds the ingest lookups from m_tracks.

        MUST be called with m_pendingMutex held, and it clears m_pending: a
        staged sample carries a track INDEX, and any change to the track list
        renumbers those. Dropping a few milliseconds of samples when the user
        adds a trace is invisible; feeding them to the wrong series is not.
    */
    void rebuildIngestIndex();
    static quint64 dbcKey(int dbcIndex, int signalKey)
    {
        return (quint64(quint32(dbcIndex)) << 32) | quint32(signalKey);
    }

    static GraphDataModel *s_instance;

    QTimer *m_refreshTimer = nullptr;
    QTimer *m_rateTimer = nullptr;

    std::vector<GraphDatabase> m_databases;

    QVector<Track> m_tracks;
    QHash<int, int> m_trackById;                    //!< stable id -> index
    int m_nextTrackId = 1;
    int m_nextColorIndex = 0;

    // -- ingest lookups, guarded by m_pendingMutex
    /*! frame key -> the database indices that have a tracked signal in it. The
        first test every incoming frame faces, and the reason an unwatched bus
        costs almost nothing. */
    QHash<quint32, QVector<int>> m_watchedFrames;

    /*!
        Latest value of every signal a math channel reads, whether or not that
        signal is itself plotted.

        A math channel's inputs usually are NOT plotted - the whole point is
        that Power is on the chart and RPM is not - so they would never be
        decoded by the track fan-out alone. rebuildIngestIndex() therefore adds
        each input's frame to m_watchedFrames and gives it a slot here;
        ingestFrames() writes the newest value into the slot as part of the
        pass it was already making. Written on the DEVICE thread under
        m_pendingMutex, read on the GUI thread under the same lock.
    */
    struct MathInput {
        double value = 0.0;
        qint64 timestampNs = 0;
        quint64 version = 0;      //!< bumped per write; a change means "re-evaluate"
    };
    QHash<quint64, int> m_mathInputSlotByDbcKey;   //!< dbcKey() -> index into m_mathInputs
    QVector<MathInput> m_mathInputs;
    /*! Per math track, the slots its arguments come from, in binding order. */
    QHash<int, QVector<int>> m_mathSlotsByTrackId;
    /*! Version vector as of the last evaluation, so a channel whose inputs have
        not moved is not re-plotted sixty times a second. */
    QHash<int, quint64> m_mathVersionByTrackId;
    MathChannelManager *m_mathChannels = nullptr;
    /*! (dbcIndex, signalKey) -> every track index fed by it. The fan-out that
        lets one signal appear on four panes. */
    QHash<quint64, QVector<int>> m_tracksByDbcKey;

    mutable QMutex m_pendingMutex;
    QVector<PendingSample> m_pending;

    qreal m_windowSeconds = 10.0;
    qreal m_latestTime = 0.0;
    int m_pointCount = 0;
    int m_sampleRate = 0;
    qint64 m_samplesSinceTick = 0;
    int m_refreshTick = 0;
    QString m_lastError;

    std::atomic_bool m_paused { false };
    std::atomic_bool m_active { false };
};
