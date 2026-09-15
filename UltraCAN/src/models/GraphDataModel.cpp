#include "GraphDataModel.h"

#include <QFileInfo>
#include <QMutexLocker>
#include <QTimer>
#include <QtCharts/QXYSeries>

#include <algorithm>
#include <cmath>

#include "dbc/DbcParser.h"
#include "models/MathChannelManager.h"

#include <QSet>

namespace {
/*! 60 Hz. Faster than the eye, slower than the bus. */
constexpr int RefreshMs = 16;
/*! Readouts do not need sixty updates a second; every twelfth is 5 Hz. */
constexpr int ReadoutEveryNRefreshes = 12;
/*! The one place the application's decimal precision is defined. */
constexpr int ValueDecimals = 3;

inline quint32 frameKeyFor(quint32 id, bool extended)
{
    return extended ? (id | 0x80000000u) : id;
}
}

GraphDataModel *GraphDataModel::s_instance = nullptr;

GraphDataModel::GraphDataModel(QObject *parent)
    : QObject(parent)
{
    if (!s_instance) {
        s_instance = this;
    } else {
        qCritical("GraphDataModel: a SECOND instance was created (%p; first was %p).",
                  static_cast<void *>(this), static_cast<void *>(s_instance));
    }

    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setInterval(RefreshMs);
    m_refreshTimer->setTimerType(Qt::PreciseTimer);
    connect(m_refreshTimer, &QTimer::timeout, this, &GraphDataModel::refresh);

    m_rateTimer = new QTimer(this);
    m_rateTimer->setInterval(1000);
    connect(m_rateTimer, &QTimer::timeout, this, [this] {
        const int rate = int(m_samplesSinceTick);
        m_samplesSinceTick = 0;
        if (rate != m_sampleRate) {
            m_sampleRate = rate;
            emit sampleRateChanged();
        }
    });
    m_rateTimer->start();
}

GraphDataModel::~GraphDataModel()
{
    if (s_instance == this)
        s_instance = nullptr;
}

GraphDataModel *GraphDataModel::instance()
{
    return s_instance;
}

// ====================================================== database management
//
// The graph's databases are ITS OWN, exactly like the analyzer's: you plot
// against candidate .dbc files without disturbing the one being authored in
// the Editor.

QVariantList GraphDataModel::loadedDbcs() const
{
    QVariantList out;
    out.reserve(int(m_databases.size()));
    for (const GraphDatabase &database : m_databases) {
        out.append(QVariantMap {
            { QStringLiteral("name"),     database.name },
            { QStringLiteral("path"),     database.path },
            { QStringLiteral("url"),      database.url },
            { QStringLiteral("messages"), database.messageCount },
            { QStringLiteral("signals"),  database.signalCount }
        });
    }
    return out;
}

int GraphDataModel::availableSignalCount() const
{
    int total = 0;
    for (const GraphDatabase &database : m_databases)
        total += database.signalCount;
    return total;
}

bool GraphDataModel::loadGraphDbc(const QUrl &url)
{
    const QString path = url.isLocalFile() ? url.toLocalFile() : url.toString();
    if (path.isEmpty()) {
        setLastError(tr("No file was given."));
        return false;
    }

    // Parse BEFORE touching the list, so a file that will not parse costs the
    // user neither their existing databases nor a half-added row.
    QString error;
    const DbcDatabase parsed = DbcParser::parseFile(path, &error);
    if (!error.isEmpty()) {
        setLastError(error);
        return false;
    }
    if (parsed.isEmpty()) {
        setLastError(tr("%1 contains no messages - is it a .dbc file?")
                     .arg(QFileInfo(path).fileName()));
        return false;
    }

    const QString fileName = QFileInfo(path).fileName();
    for (const GraphDatabase &existing : m_databases) {
        if (existing.path == path) {
            setLastError(tr("%1 is already loaded.").arg(fileName));
            return false;
        }
    }

    GraphDatabase database;
    database.name = parsed.name.isEmpty() ? fileName : parsed.name;
    database.path = path;
    database.url = url;
    database.messageCount = int(parsed.messages.size());
    for (const DbcMessageData &message : parsed.messages)
        database.signalCount += int(message.signalList.size());
    database.parsed = parsed;
    database.decoder = std::make_unique<CanDecoder>();
    database.decoder->setDatabase(parsed);

    m_databases.push_back(std::move(database));

    setLastError(QString());
    emit databasesChanged();
    qInfo("Graph DBC: +%s (%d messages, %d signals); %lld loaded",
          qPrintable(fileName), m_databases.back().messageCount,
          m_databases.back().signalCount, qint64(m_databases.size()));
    return true;
}

void GraphDataModel::removeGraphDbc(int dbcIndex)
{
    if (dbcIndex < 0 || dbcIndex >= int(m_databases.size()))
        return;

    // Every track fed by this database is about to point at a freed decoder,
    // and every track AFTER it has its dbcIndex shifted by one. Drop the first
    // group and renumber the second - in that order.
    for (int i = int(m_tracks.size()) - 1; i >= 0; --i) {
        if (m_tracks[i].dbcIndex == dbcIndex)
            removeTrackAt(i);
    }
    for (Track &track : m_tracks) {
        if (track.dbcIndex > dbcIndex)
            --track.dbcIndex;
    }

    const QString name = m_databases[size_t(dbcIndex)].name;
    m_databases.erase(m_databases.begin() + dbcIndex);

    {
        QMutexLocker locker(&m_pendingMutex);
        rebuildIngestIndex();
    }

    updateActive();
    emit databasesChanged();
    emit tracksChanged();
    setLastError(tr("Removed %1 - its traces were dropped.").arg(name));
}

void GraphDataModel::clearGraphDbcs()
{
    if (m_databases.empty())
        return;
    untrackAll();
    m_databases.clear();
    emit databasesChanged();
    setLastError(tr("All databases removed."));
}

QVariantMap GraphDataModel::findSignal(const QString &dbcName,
                                       const QString &messageName,
                                       const QString &signalName) const
{
    QVariantMap miss { { QStringLiteral("found"), false } };
    if (messageName.isEmpty() || signalName.isEmpty())
        return miss;

    for (size_t d = 0; d < m_databases.size(); ++d) {
        const GraphDatabase &database = m_databases[d];
        // An empty dbcName means "wherever it lives". Matching on the display
        // name rather than the path is deliberate: the same database moved to
        // a different folder is still the same database.
        if (!dbcName.isEmpty() && database.name != dbcName)
            continue;

        const DbcDatabase &parsed = database.parsed;
        const int messageTotal = int(parsed.messages.size());
        for (int m = 0; m < messageTotal; ++m) {
            const DbcMessageData &message = parsed.messages.at(m);
            if (message.name != messageName)
                continue;

            const int signalTotal = int(message.signalList.size());
            for (int sig = 0; sig < signalTotal; ++sig) {
                if (message.signalList.at(sig).name != signalName)
                    continue;
                return QVariantMap {
                    { QStringLiteral("found"),        true },
                    { QStringLiteral("dbcIndex"),     int(d) },
                    { QStringLiteral("messageIndex"), m },
                    { QStringLiteral("signalIndex"),  sig }
                };
            }
        }
    }
    return miss;
}

QVariantList GraphDataModel::signalCatalog() const
{
    QVariantList out;
    for (size_t d = 0; d < m_databases.size(); ++d) {
        const GraphDatabase &database = m_databases[d];
        if (!database.decoder)
            continue;

        QVariantList messages;
        const DbcDatabase &parsed = database.parsed;
        const int messageTotal = int(parsed.messages.size());
        for (int m = 0; m < messageTotal; ++m) {
            const DbcMessageData &message = parsed.messages.at(m);

            QVariantList signalList;
            const int signalTotal = int(message.signalList.size());
            for (int s = 0; s < signalTotal; ++s) {
                const DbcSignalData &signalData = message.signalList.at(s);
                signalList.append(QVariantMap {
                    { QStringLiteral("signalIndex"), s },
                    { QStringLiteral("name"),        signalData.name },
                    { QStringLiteral("unit"),        signalData.unit }
                });
            }

            messages.append(QVariantMap {
                { QStringLiteral("messageIndex"), m },
                { QStringLiteral("name"),         message.name },
                { QStringLiteral("idText"),       QStringLiteral("0x%1")
                      .arg(message.id, message.extended ? 8 : 3, 16, QLatin1Char('0')).toUpper() },
                { QStringLiteral("signalList"),   signalList }
            });
        }

        out.append(QVariantMap {
            { QStringLiteral("dbcIndex"), int(d) },
            { QStringLiteral("dbcName"),  database.name },
            { QStringLiteral("messages"), messages }
        });
    }
    return out;
}

QVariantList GraphDataModel::searchSignals(const QString &needle, int limit) const
{
    QVariantList out;
    const QString filter = needle.trimmed();

    for (size_t d = 0; d < m_databases.size() && int(out.size()) < limit; ++d) {
        const GraphDatabase &database = m_databases[d];
        if (!database.decoder)
            continue;

        const DbcDatabase &parsed = database.parsed;
        const int messageTotal = int(parsed.messages.size());
        for (int m = 0; m < messageTotal && int(out.size()) < limit; ++m) {
            const DbcMessageData &message = parsed.messages.at(m);
            const int signalTotal = int(message.signalList.size());
            for (int s = 0; s < signalTotal && int(out.size()) < limit; ++s) {
                const DbcSignalData &signalData = message.signalList.at(s);

                // Match the message name too: people look for "the brake
                // message" as often as for a signal by name.
                if (!filter.isEmpty()
                    && !signalData.name.contains(filter, Qt::CaseInsensitive)
                    && !message.name.contains(filter, Qt::CaseInsensitive)) {
                    continue;
                }

                out.append(QVariantMap {
                    { QStringLiteral("dbcIndex"),     int(d) },
                    { QStringLiteral("messageIndex"), m },
                    { QStringLiteral("signalIndex"),  s },
                    { QStringLiteral("dbcName"),      database.name },
                    { QStringLiteral("messageName"),  message.name },
                    { QStringLiteral("signalName"),   signalData.name },
                    { QStringLiteral("unit"),         signalData.unit },
                    { QStringLiteral("label"),        QStringLiteral("%1 . %2")
                          .arg(message.name, signalData.name) }
                });
            }
        }
    }
    return out;
}

// ================================================================= tracking
//
// A track is a SERIES, not a signal. Two panes plotting EngineSpeed are two
// tracks sharing one decoded value - which is the whole reason the old
// key-indexed design had to go.

int GraphDataModel::trackSignal(int dbcIndex, int messageIndex, int signalIndex,
                                QObject *series, const QString &paneId)
{
    if (dbcIndex < 0 || dbcIndex >= int(m_databases.size())) {
        setLastError(tr("Database %1 is not loaded.").arg(dbcIndex));
        return -1;
    }

    auto *xySeries = qobject_cast<QXYSeries *>(series);
    if (!xySeries) {
        setLastError(tr("The chart did not hand back a usable series object."));
        return -1;
    }

    GraphDatabase &database = m_databases[size_t(dbcIndex)];
    const int key = database.decoder->signalKey(messageIndex, signalIndex);
    if (key < 0) {
        setLastError(tr("Signal %1:%2 is not in %3's decode table.")
                     .arg(messageIndex).arg(signalIndex).arg(database.name));
        return -1;
    }
    if (messageIndex < 0 || messageIndex >= int(database.parsed.messages.size())) {
        setLastError(tr("Message %1 is out of range for %2.")
                     .arg(messageIndex).arg(database.name));
        return -1;
    }

    const DbcMessageData &message = database.parsed.messages.at(messageIndex);
    if (signalIndex < 0 || signalIndex >= int(message.signalList.size())) {
        setLastError(tr("Signal %1 is out of range for %2.")
                     .arg(signalIndex).arg(message.name));
        return -1;
    }
    const DbcSignalData &signalData = message.signalList.at(signalIndex);

    setLastError(QString());

    Track track;
    track.id = m_nextTrackId++;
    track.dbcIndex = dbcIndex;
    track.key = key;
    track.frameKey = frameKeyFor(message.id, message.extended);
    track.messageIndex = messageIndex;
    track.signalIndex = signalIndex;
    track.paneId = paneId;
    track.signalName = signalData.name;
    track.messageName = message.name;
    track.dbcName = database.name;
    track.unit = signalData.unit;
    track.minimum = signalData.minimum;
    track.maximum = signalData.maximum;
    // Runs forward across the whole session rather than restarting per pane, so
    // the same trace is not drawn in two different colours on two panes.
    track.colorIndex = m_nextColorIndex++;
    track.series = xySeries;
    track.points.reserve(2048);

    const int trackId = track.id;
    m_tracks.append(track);

    {
        QMutexLocker locker(&m_pendingMutex);
        rebuildIngestIndex();
    }

    // The series belongs to a ChartView, and a pane closing (or the page
    // navigating away, or a detached window being shut) destroys it. Drop the
    // track with it rather than leaving refresh() a dangling pointer.
    connect(xySeries, &QObject::destroyed, this, [this, trackId](QObject *) {
        const int index = m_trackById.value(trackId, -1);
        if (index < 0)
            return;
        m_tracks[index].series = nullptr;      // already gone; do not touch it
        removeTrackAt(index);
    });

    updateActive();
    emit tracksChanged();

    if (!m_refreshTimer->isActive())
        m_refreshTimer->start();
    return trackId;
}

// ============================================================ math channels

void GraphDataModel::setMathChannelManager(MathChannelManager *manager)
{
    if (m_mathChannels == manager)
        return;

    if (m_mathChannels)
        disconnect(m_mathChannels, nullptr, this, nullptr);

    m_mathChannels = manager;
    if (!m_mathChannels)
        return;

    // A channel the user deletes must not leave a track evaluating an id the
    // manager no longer knows - evaluate() would refuse it silently and the
    // trace would freeze mid-air rather than disappear.
    connect(m_mathChannels, &MathChannelManager::channelsChanged, this, [this] {
        bool removed = false;
        for (int i = int(m_tracks.size()) - 1; i >= 0; --i) {
            const int channelId = m_tracks[i].mathChannelId;
            if (channelId < 0)
                continue;
            if (m_mathChannels->channelAt(channelId).isEmpty()) {
                removeTrackAt(i);
                removed = true;
            }
        }
        if (removed)
            emit tracksChanged();
        emit mathChannelsChanged();
    });

    // Re-pointing a variable at a different signal changes which frames have
    // to be decoded, so the ingest index is no longer the right one.
    connect(m_mathChannels, &MathChannelManager::bindingsChanged, this, [this] {
        QMutexLocker locker(&m_pendingMutex);
        rebuildIngestIndex();
    });

    emit mathChannelsChanged();
}

QVariantList GraphDataModel::mathChannels() const
{
    if (!m_mathChannels)
        return {};
    return m_mathChannels->channels();
}

int GraphDataModel::trackMathChannel(int channelId, QObject *series, const QString &paneId)
{
    if (!m_mathChannels) {
        setLastError(tr("Math channels are not available in this build."));
        return -1;
    }

    const QVariantMap channel = m_mathChannels->channelAt(channelId);
    if (channel.isEmpty()) {
        setLastError(tr("Math channel %1 does not exist.").arg(channelId));
        return -1;
    }
    if (!m_mathChannels->isValid(channelId)) {
        setLastError(tr("Math channel \"%1\" does not compile - fix its formula "
                        "before plotting it.").arg(m_mathChannels->nameOf(channelId)));
        return -1;
    }

    auto *xySeries = qobject_cast<QXYSeries *>(series);
    if (!xySeries) {
        setLastError(tr("The chart did not hand back a usable series object."));
        return -1;
    }

    setLastError(QString());

    Track track;
    track.id = m_nextTrackId++;
    track.mathChannelId = channelId;
    // dbcIndex / key / frameKey stay unset: nothing on the bus feeds this track
    // directly. Its inputs are watched separately by rebuildIngestIndex().
    track.paneId = paneId;
    track.signalName = m_mathChannels->nameOf(channelId);
    track.messageName = tr("Math");
    track.dbcName = tr("Virtual");
    track.unit = m_mathChannels->unitOf(channelId);
    // A formula has no DBC min/max. Leaving both at 0 lets the pane fall back
    // to auto-scale, which is the only honest range for an arbitrary expression.
    track.colorIndex = m_nextColorIndex++;
    track.series = xySeries;
    track.points.reserve(2048);

    const int trackId = track.id;
    m_tracks.append(track);

    {
        QMutexLocker locker(&m_pendingMutex);
        rebuildIngestIndex();
    }

    connect(xySeries, &QObject::destroyed, this, [this, trackId](QObject *) {
        const int index = m_trackById.value(trackId, -1);
        if (index < 0)
            return;
        m_tracks[index].series = nullptr;
        removeTrackAt(index);
    });

    updateActive();
    emit tracksChanged();

    if (!m_refreshTimer->isActive())
        m_refreshTimer->start();
    return trackId;
}

void GraphDataModel::evaluateMathTracks()
{
    if (!m_mathChannels || m_mathSlotsByTrackId.isEmpty())
        return;

    QVector<double> arguments;
    // A channel plotted on two panes is two tracks and one value: the value
    // stream must not be announced twice for one sample.
    QSet<int> announced;

    for (Track &track : m_tracks) {
        if (track.mathChannelId < 0)
            continue;

        const auto slotsIt = m_mathSlotsByTrackId.constFind(track.id);
        if (slotsIt == m_mathSlotsByTrackId.constEnd())
            continue;
        const QVector<int> &inputSlots = slotsIt.value();
        if (inputSlots.isEmpty())
            continue;

        // A -1 slot is an input whose database is not loaded. Evaluating with a
        // stand-in zero would draw a confident wrong line, so the channel stays
        // silent until every input actually exists.
        bool complete = true;
        quint64 stamp = 0;
        qint64 newest = 0;
        for (int slot : inputSlots) {
            if (slot < 0) {
                complete = false;
                break;
            }
            const MathInput &input = m_mathInputs.at(slot);
            if (input.version == 0) {          // never carried a value yet
                complete = false;
                break;
            }
            stamp += input.version;
            newest = qMax(newest, input.timestampNs);
        }
        if (!complete)
            continue;

        // Re-evaluating an unchanged formula would append 60 identical points a
        // second and push the real ones out of the window.
        if (m_mathVersionByTrackId.value(track.id, 0) == stamp)
            continue;
        m_mathVersionByTrackId.insert(track.id, stamp);

        arguments.resize(inputSlots.size());
        for (int i = 0; i < inputSlots.size(); ++i)
            arguments[i] = m_mathInputs.at(inputSlots.at(i)).value;

        double result = 0.0;
        if (!m_mathChannels->evaluate(track.mathChannelId, arguments, &result))
            continue;                          // non-finite or threw: leave a gap

        const qreal seconds = double(newest) / 1'000'000'000.0;
        track.points.append(QPointF(seconds, result));
        track.lastValue = result;
        if (seconds > m_latestTime)
            m_latestTime = seconds;

        if (!announced.contains(track.mathChannelId)) {
            announced.insert(track.mathChannelId);
            m_mathChannels->publishSample(track.mathChannelId, result, seconds);
        }
    }
}

void GraphDataModel::untrack(int trackId)
{
    const int index = m_trackById.value(trackId, -1);
    if (index >= 0)
        removeTrackAt(index);
}

void GraphDataModel::untrackPane(const QString &paneId)
{
    // Backwards: removeTrackAt() compacts the vector, so a forward loop would
    // step over the element that slid into the gap.
    bool removed = false;
    for (int i = int(m_tracks.size()) - 1; i >= 0; --i) {
        if (m_tracks[i].paneId == paneId) {
            removeTrackAt(i);
            removed = true;
        }
    }
    if (removed)
        emit tracksChanged();
}

void GraphDataModel::untrackAll()
{
    if (m_tracks.isEmpty())
        return;

    for (Track &track : m_tracks) {
        if (track.series) {
            disconnect(track.series, nullptr, this, nullptr);
            track.series->clear();
        }
    }
    m_tracks.clear();
    m_trackById.clear();

    {
        QMutexLocker locker(&m_pendingMutex);
        rebuildIngestIndex();
    }

    m_pointCount = 0;
    updateActive();
    emit tracksChanged();
    emit refreshed();
}

void GraphDataModel::removeTrackAt(int index)
{
    if (index < 0 || index >= int(m_tracks.size()))
        return;

    Track &track = m_tracks[index];
    if (track.series) {
        disconnect(track.series, nullptr, this, nullptr);
        track.series->clear();
    }
    m_tracks.remove(index);

    {
        QMutexLocker locker(&m_pendingMutex);
        rebuildIngestIndex();
    }

    updateActive();
    emit tracksChanged();
}

void GraphDataModel::rebuildIngestIndex()
{
    // Caller holds m_pendingMutex - see the declaration.
    m_watchedFrames.clear();
    m_tracksByDbcKey.clear();
    m_trackById.clear();
    m_mathInputSlotByDbcKey.clear();
    m_mathInputs.clear();
    m_mathSlotsByTrackId.clear();
    m_mathVersionByTrackId.clear();

    for (int i = 0; i < int(m_tracks.size()); ++i) {
        const Track &track = m_tracks[i];
        m_trackById.insert(track.id, i);

        // A math track has no frame of its own. Falling through here would
        // register a watch on frameKey 0 against dbcIndex -1, which matches
        // nothing and costs a hash lookup on every frame of a live bus.
        if (track.mathChannelId >= 0)
            continue;

        QVector<int> &dbcIndices = m_watchedFrames[track.frameKey];
        if (!dbcIndices.contains(track.dbcIndex))
            dbcIndices.append(track.dbcIndex);

        m_tracksByDbcKey[dbcKey(track.dbcIndex, track.key)].append(i);
    }

    // -- second pass: the INPUTS of every math track.
    //
    // Done after the loop above rather than inside it because two channels can
    // read the same signal, and that signal deserves one slot, one watch and
    // one decode - not one per channel.
    if (m_mathChannels) {
        for (const Track &track : m_tracks) {
            if (track.mathChannelId < 0)
                continue;

            QVector<int> inputSlots;
            const auto bindings = m_mathChannels->bindingsOf(track.mathChannelId);
            for (const auto &binding : bindings) {
                const QVariantMap hit = findSignal(binding.dbcName, binding.messageName,
                                                   binding.signalName);
                if (!hit.value(QStringLiteral("found")).toBool()) {
                    // The database this input came from is not loaded (yet).
                    // -1 marks the argument as unavailable; evaluateMathTracks
                    // skips the channel rather than feeding it a stale zero.
                    inputSlots.append(-1);
                    continue;
                }
                const int inputDbc = hit.value(QStringLiteral("dbcIndex")).toInt();
                const int messageIndex = hit.value(QStringLiteral("messageIndex")).toInt();
                const int signalIndex = hit.value(QStringLiteral("signalIndex")).toInt();

                const GraphDatabase &database = m_databases[size_t(inputDbc)];
                const int key = database.decoder ? database.decoder->signalKey(messageIndex,
                                                                               signalIndex)
                                                 : -1;
                if (key < 0 || messageIndex >= int(database.parsed.messages.size())) {
                    inputSlots.append(-1);
                    continue;
                }
                const DbcMessageData &message = database.parsed.messages.at(messageIndex);

                const quint64 inputKey = dbcKey(inputDbc, key);
                auto slot = m_mathInputSlotByDbcKey.constFind(inputKey);
                int slotIndex;
                if (slot == m_mathInputSlotByDbcKey.constEnd()) {
                    slotIndex = m_mathInputs.size();
                    m_mathInputs.append(MathInput {});
                    m_mathInputSlotByDbcKey.insert(inputKey, slotIndex);
                } else {
                    slotIndex = slot.value();
                }
                inputSlots.append(slotIndex);

                // The input has to be DECODED even though nothing plots it.
                QVector<int> &dbcIndices = m_watchedFrames[frameKeyFor(message.id,
                                                                       message.extended)];
                if (!dbcIndices.contains(inputDbc))
                    dbcIndices.append(inputDbc);
            }
            m_mathSlotsByTrackId.insert(track.id, inputSlots);
        }
    }

    // Staged samples carry track INDICES, which the rebuild above just
    // renumbered. Dropping a frame's worth of samples is invisible; feeding
    // them to the wrong series is not.
    m_pending.clear();
}

void GraphDataModel::updateActive()
{
    const bool active = !m_tracks.isEmpty();
    if (m_active.exchange(active) == active)
        return;
    if (!active && m_refreshTimer->isActive())
        m_refreshTimer->stop();
    emit activeChanged(active);
}

void GraphDataModel::rebaseTo(qreal seconds)
{
    for (Track &track : m_tracks) {
        track.points.clear();
        track.hasData = false;
        if (track.series)
            track.series->clear();
    }
    {
        QMutexLocker locker(&m_pendingMutex);
        // Samples staged before the jump belong to the timeline we just left.
        m_pending.clear();
    }

    m_latestTime = qMax(0.0, seconds);
    m_pointCount = 0;
    emit refreshed();
}

void GraphDataModel::clear()
{
    for (Track &track : m_tracks) {
        track.points.clear();
        track.lastValue = 0.0;
        if (track.series)
            track.series->clear();
    }
    {
        QMutexLocker locker(&m_pendingMutex);
        m_pending.clear();
    }
    m_latestTime = 0.0;
    m_pointCount = 0;
    emit refreshed();
}

// ============================================================ pane readouts

QVariantList GraphDataModel::tracksForPane(const QString &paneId) const
{
    QVariantList out;
    for (const Track &track : m_tracks) {
        if (track.paneId != paneId)
            continue;
        out.append(QVariantMap {
            { QStringLiteral("trackId"),     track.id },
            // How a serialiser tells the two kinds apart. A math track has no
            // dbcIndex/messageIndex/signalIndex worth writing down - it is
            // restored by channel NAME through trackMathChannel().
            { QStringLiteral("isVirtual"),   track.mathChannelId >= 0 },
            { QStringLiteral("channelId"),   track.mathChannelId },
            // The coordinates the trace was created from. Detaching the graph
            // into its own window tears the charts down and rebuilds them, and
            // these are what let the view put the same traces back afterwards
            // instead of handing the user four empty panes.
            { QStringLiteral("dbcIndex"),     track.dbcIndex },
            { QStringLiteral("messageIndex"), track.messageIndex },
            { QStringLiteral("signalIndex"),  track.signalIndex },
            { QStringLiteral("name"),        track.signalName },
            { QStringLiteral("messageName"), track.messageName },
            { QStringLiteral("dbcName"),     track.dbcName },
            { QStringLiteral("unit"),        track.unit },
            { QStringLiteral("minimum"),     track.minimum },
            { QStringLiteral("maximum"),     track.maximum },
            { QStringLiteral("colorIndex"),  track.colorIndex }
        });
    }
    return out;
}

double GraphDataModel::lastValue(int trackId) const
{
    const int index = m_trackById.value(trackId, -1);
    return index < 0 ? 0.0 : m_tracks[index].lastValue;
}

QString GraphDataModel::lastValueText(int trackId) const
{
    const int index = m_trackById.value(trackId, -1);
    if (index < 0)
        return {};

    const Track &track = m_tracks[index];
    // ALWAYS three decimals, including trailing zeros. A column of readouts
    // that switches between "3500" and "3500.125" as the value happens to land
    // on an integer jitters horizontally and cannot be scanned; fixed width
    // beats shortest representation everywhere in this application.
    const QString number = QString::number(track.lastValue, 'f', ValueDecimals);
    return track.unit.isEmpty() ? number : QStringLiteral("%1 %2").arg(number, track.unit);
}

qreal GraphDataModel::paneMinimum(const QString &paneId) const
{
    bool found = false;
    qreal low = 0.0;
    for (const Track &track : m_tracks) {
        if (track.paneId != paneId)
            continue;
        low = found ? qMin(low, qreal(track.minimum)) : qreal(track.minimum);
        found = true;
    }
    return found ? low : 0.0;
}

qreal GraphDataModel::paneMaximum(const QString &paneId) const
{
    bool found = false;
    qreal high = 0.0;
    for (const Track &track : m_tracks) {
        if (track.paneId != paneId)
            continue;
        high = found ? qMax(high, qreal(track.maximum)) : qreal(track.maximum);
        found = true;
    }
    // A database that declares min == max (very common for raw counters) would
    // give a zero-height axis, which Qt Charts renders as an empty box.
    return (found && high > paneMinimum(paneId)) ? high : paneMinimum(paneId) + 1.0;
}

bool GraphDataModel::paneHasData(const QString &paneId) const
{
    for (const Track &track : m_tracks) {
        if (track.paneId == paneId && track.hasData)
            return true;
    }
    return false;
}

bool GraphDataModel::paneDataRange(const QString &paneId, QVariantMap *out) const
{
    bool found = false;
    double low = 0.0;
    double high = 0.0;

    for (const Track &track : m_tracks) {
        if (track.paneId != paneId || !track.hasData)
            continue;
        low = found ? qMin(low, track.dataMinimum) : track.dataMinimum;
        high = found ? qMax(high, track.dataMaximum) : track.dataMaximum;
        found = true;
    }
    if (!found)
        return false;

    // A dead-flat trace has zero extent, which Qt Charts draws as an empty
    // box. Give it something to sit in the middle of.
    if (qFuzzyIsNull(high - low)) {
        const double pad = qMax(0.5, qAbs(high) * 0.05);
        low -= pad;
        high += pad;
    }

    if (out) {
        out->insert(QStringLiteral("minimum"), low);
        out->insert(QStringLiteral("maximum"), high);
    }
    return true;
}

qreal GraphDataModel::paneDataMinimum(const QString &paneId) const
{
    QVariantMap range;
    return paneDataRange(paneId, &range) ? range.value(QStringLiteral("minimum")).toReal()
                                         : paneMinimum(paneId);
}

qreal GraphDataModel::paneDataMaximum(const QString &paneId) const
{
    QVariantMap range;
    return paneDataRange(paneId, &range) ? range.value(QStringLiteral("maximum")).toReal()
                                         : paneMaximum(paneId);
}

// =================================================================== ingest
//
// CAN device thread. The decode happens HERE, with this model's own decoders,
// because the samples the bus manager produces carry keys from ITS decoder and
// those mean nothing to us.

void GraphDataModel::ingestFrames(const QVector<CanFrame> &frames)
{
    if (frames.isEmpty() || m_paused.load(std::memory_order_relaxed))
        return;
    if (!m_active.load(std::memory_order_relaxed))
        return;

    // Reused across calls so a busy bus does not allocate per batch.
    thread_local QVector<DecodedSample> samples;

    QMutexLocker locker(&m_pendingMutex);
    if (m_watchedFrames.isEmpty())
        return;

    for (const CanFrame &frame : frames) {
        // One hash lookup decides whether this frame is worth any work at all.
        // On a 5000 fps bus with three traces plotted, almost every frame stops
        // right here.
        const auto watched = m_watchedFrames.constFind(frameKeyFor(frame.id, frame.extended));
        if (watched == m_watchedFrames.constEnd())
            continue;

        for (int dbcIndex : *watched) {
            if (dbcIndex < 0 || dbcIndex >= int(m_databases.size()))
                continue;
            const CanDecoder *decoder = m_databases[size_t(dbcIndex)].decoder.get();
            if (!decoder)
                continue;

            samples.clear();
            decoder->decode(frame, samples);

            for (const DecodedSample &sample : samples) {
                const quint64 key = dbcKey(dbcIndex, sample.key);

                // Math inputs first, and cheaply: one hash lookup and, on a
                // hit, three stores. No allocation and no evaluation - the
                // JavaScript runs on the GUI thread at repaint rate, never
                // here on the device thread.
                const auto slot = m_mathInputSlotByDbcKey.constFind(key);
                if (slot != m_mathInputSlotByDbcKey.constEnd()) {
                    MathInput &input = m_mathInputs[slot.value()];
                    input.value = sample.value;
                    input.timestampNs = sample.timestampNs;
                    ++input.version;
                }

                const auto tracked = m_tracksByDbcKey.constFind(key);
                if (tracked == m_tracksByDbcKey.constEnd())
                    continue;
                // The fan-out: one decoded value, one staged sample per pane
                // that asked for it.
                for (int trackIndex : *tracked)
                    m_pending.append(PendingSample { trackIndex, sample.timestampNs, sample.value });
            }
        }
    }
}

// ================================================================== repaint

void GraphDataModel::refresh()
{
    QVector<PendingSample> batch;
    {
        QMutexLocker locker(&m_pendingMutex);
        batch.swap(m_pending);
    }

    if (!batch.isEmpty()) {
        m_samplesSinceTick += batch.size();

        for (const PendingSample &sample : batch) {
            // The index was resolved on the producer thread against the track
            // list as it stood then. rebuildIngestIndex() clears m_pending on
            // every change, so a stale index cannot reach here - the bounds
            // check is belt and braces.
            if (sample.trackIndex < 0 || sample.trackIndex >= int(m_tracks.size()))
                continue;
            Track &track = m_tracks[sample.trackIndex];
            const qreal seconds = double(sample.timestampNs) / 1'000'000'000.0;
            track.points.append(QPointF(seconds, sample.value));
            track.lastValue = sample.value;
            if (seconds > m_latestTime)
                m_latestTime = seconds;
        }
    }

    if (m_tracks.isEmpty())
        return;

    // Virtual traces are produced here, on the GUI thread: QJSEngine is not
    // thread-safe and frames decode on the device thread.
    evaluateMathTracks();

    // Trim to the visible window, then hand each series its whole point list in
    // one call. This is the only place that touches Qt Charts.
    const qreal cutoff = m_latestTime - m_windowSeconds;
    int total = 0;
    for (Track &track : m_tracks) {
        int stale = 0;
        while (stale < track.points.size() && track.points.at(stale).x() < cutoff)
            ++stale;
        if (stale > 0)
            track.points.remove(0, stale);

        if (track.series)
            track.series->replace(track.points);
        total += track.points.size();
    }
    m_pointCount = total;

    // Data extent for auto-scale. Every twelfth repaint is 5 Hz, which is as
    // fast as an axis should move anyway - rescaling at 60 Hz makes a trace
    // look like it is breathing.
    if (m_refreshTick % ReadoutEveryNRefreshes == 0) {
        for (Track &track : m_tracks) {
            if (track.points.isEmpty()) {
                track.hasData = false;
                continue;
            }
            double low = track.points.first().y();
            double high = low;
            for (const QPointF &point : std::as_const(track.points)) {
                low = qMin(low, point.y());
                high = qMax(high, point.y());
            }
            track.dataMinimum = low;
            track.dataMaximum = high;
            track.hasData = true;
        }
    }

    ++m_refreshTick;
    emit refreshed();
}

// ================================================================ plain state

void GraphDataModel::setWindowSeconds(qreal seconds)
{
    const qreal clamped = qBound(0.5, seconds, 600.0);
    if (qFuzzyIsNull(m_windowSeconds - clamped))
        return;
    m_windowSeconds = clamped;
    emit windowSecondsChanged();
}

void GraphDataModel::setPaused(bool paused)
{
    if (m_paused.exchange(paused) == paused)
        return;
    if (paused) {
        // Drop whatever the producer staged before it noticed, so resuming does
        // not paste a burst of samples with old timestamps onto the traces.
        QMutexLocker locker(&m_pendingMutex);
        m_pending.clear();
    }
    emit pausedChanged();
}

void GraphDataModel::setLastError(const QString &message)
{
    if (m_lastError == message)
        return;
    m_lastError = message;
    emit lastErrorChanged();
}
