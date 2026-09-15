#include "DbcFlatModel.h"

#include "DbcModel.h"

#include <QTimer>
#include <QtGlobal>

#include <cmath>
#include <utility>

namespace {

/*! Flush cadence for the live column. See the class comment for why 5 Hz. */
constexpr int LiveFlushMs = 200;

/*! Smoothing for the measured frame rate. A raw count/window figure jitters by
    a frame either side and makes the column look broken; an EMA settles in
    about a second without hiding a real change. */
constexpr double RateSmoothing = 0.35;

QString hexId(quint32 id, bool extended)
{
    // "A7h" - the notation the DAQ tools this grid is modelled on use. Width is
    // fixed per format so the column stays aligned: 3 nibbles for an 11-bit id,
    // 8 for a 29-bit one.
    return QStringLiteral("%1h").arg(
        QStringLiteral("%1").arg(id, extended ? 8 : 3, 16, QLatin1Char('0')).toUpper());
}

quint32 frameKeyOf(quint32 id, bool extended)
{
    return extended ? (id | 0x80000000u) : id;
}

} // namespace

DbcFlatModel *DbcFlatModel::s_instance = nullptr;

DbcFlatModel::DbcFlatModel(QObject *parent)
    : QAbstractListModel(parent)
{
    if (s_instance)
        qCritical("DbcFlatModel: a second instance was created - QML will bind to the wrong one");
    s_instance = this;

    // Coalescing matters more than it looks: loading a database emits several
    // structural signals in a row, and each one would otherwise reset the model.
    m_rebuildTimer = new QTimer(this);
    m_rebuildTimer->setSingleShot(true);
    m_rebuildTimer->setInterval(0);
    connect(m_rebuildTimer, &QTimer::timeout, this, &DbcFlatModel::rebuild);

    m_liveTimer = new QTimer(this);
    m_liveTimer->setInterval(LiveFlushMs);
    connect(m_liveTimer, &QTimer::timeout, this, &DbcFlatModel::flushLive);
}

DbcFlatModel::~DbcFlatModel()
{
    if (s_instance == this)
        s_instance = nullptr;
}

DbcFlatModel *DbcFlatModel::instance()
{
    return s_instance;
}

// ------------------------------------------------------------------- wiring

void DbcFlatModel::setSource(DbcModel *source)
{
    if (m_source == source)
        return;

    if (m_source)
        m_source->disconnect(this);

    m_source = source;

    if (m_source) {
        // Everything funnels into one coalesced rebuild(), which then decides
        // between dataChanged and a reset by comparing structures. Listening to
        // dataChanged as well is what picks up an expand/collapse: DbcModel
        // reports that as a role change and deliberately NOT as a database
        // change, because collapsing a message must not mark the file modified.
        connect(m_source, &DbcModel::countChanged,    this, &DbcFlatModel::scheduleRebuild);
        connect(m_source, &DbcModel::loaded,          this, &DbcFlatModel::scheduleRebuild);
        connect(m_source, &DbcModel::databaseChanged, this, &DbcFlatModel::scheduleRebuild);
        connect(m_source, &DbcModel::modelReset,      this, &DbcFlatModel::scheduleRebuild);
        connect(m_source, &DbcModel::dataChanged,     this, &DbcFlatModel::scheduleRebuild);
        connect(m_source, &DbcModel::rowsInserted,    this, &DbcFlatModel::scheduleRebuild);
        connect(m_source, &DbcModel::rowsRemoved,     this, &DbcFlatModel::scheduleRebuild);
    }

    rebuild();
}

void DbcFlatModel::setDecoder(const CanDecoder *decoder)
{
    m_decoder = decoder;
    resolveDecoderKeys();
}

void DbcFlatModel::setLiveValuesEnabled(bool enabled)
{
    if (m_liveEnabled == enabled)
        return;
    m_liveEnabled = enabled;

    if (m_liveEnabled) {
        m_liveWindow.restart();
        m_liveTimer->start();
    } else {
        m_liveTimer->stop();
        {
            QMutexLocker locker(&m_liveMutex);
            m_pendingValues.clear();
            m_pendingCounts.clear();
        }
        // Drop the readings rather than freezing them on screen. A stale number
        // with no indication that it stopped updating is worse than a dash.
        m_liveValues.clear();
        m_liveRates.clear();
        if (!m_rows.isEmpty()) {
            emit dataChanged(index(0), index(m_rows.size() - 1),
                             { LiveValueRole, LiveTextRole, LiveFractionRole,
                               FrequencyTextRole });
        }
    }

    emit liveValuesEnabledChanged();
}

// ------------------------------------------------------------------- rows

QVector<DbcFlatModel::Row> DbcFlatModel::buildRows() const
{
    QVector<Row> rows;
    if (!m_source)
        return rows;

    const int messageCount = m_source->count();
    rows.reserve(messageCount * 4);

    // One running counter across the whole database rather than a per-message
    // one, so two signals that sit next to each other never get the same colour.
    int colorIndex = 0;

    for (int m = 0; m < messageCount; ++m) {
        DbcMessageObject *message = m_source->message(m);
        if (!message)
            continue;

        Row row;
        row.kind = MessageRow;
        row.messageIndex = m;
        row.signalIndex = -1;
        row.colorIndex = colorIndex++;
        rows.append(row);

        DbcSignalListModel *list = message->signalList();
        const int signalCount = list ? list->count() : 0;
        for (int sIndex = 0; sIndex < signalCount; ++sIndex) {
            Row child;
            child.kind = SignalRow;
            child.messageIndex = m;
            child.signalIndex = sIndex;
            child.colorIndex = colorIndex++;
            // A collapsed message contributes its header row only. Building the
            // hidden children anyway and filtering in QML would defeat the
            // point of flattening.
            if (message->expanded())
                rows.append(child);
            // The counter still advances for a hidden signal, so a colour does
            // not change just because a message above was collapsed.
        }
    }

    return rows;
}

void DbcFlatModel::scheduleRebuild()
{
    if (!m_rebuildTimer->isActive())
        m_rebuildTimer->start();
}

void DbcFlatModel::rebuild()
{
    QVector<Row> next = buildRows();

    bool structureChanged = next.size() != m_rows.size();
    if (!structureChanged) {
        for (int i = 0; i < next.size(); ++i) {
            if (!next.at(i).sameAs(m_rows.at(i))) {
                structureChanged = true;
                break;
            }
        }
    }

    if (structureChanged) {
        beginResetModel();
        m_rows = std::move(next);
        resolveDecoderKeys();
        endResetModel();
        emit countChanged();
        return;
    }

    // Same rows, different content: refresh in place so the view keeps its
    // selection and scroll position.
    m_rows = std::move(next);
    resolveDecoderKeys();
    if (!m_rows.isEmpty())
        emit dataChanged(index(0), index(m_rows.size() - 1));
}

void DbcFlatModel::decoderRebuilt()
{
    if (m_rows.isEmpty())
        return;

    // The rows themselves have not moved - only the keys they point through -
    // so this is a data refresh, never a reset. See the header note for why it
    // has to happen at all.
    resolveDecoderKeys();
    emit dataChanged(index(0), index(m_rows.size() - 1));
}

void DbcFlatModel::resolveDecoderKeys()
{
    for (Row &row : m_rows) {
        row.decoderKey = (m_decoder && row.kind == SignalRow)
                       ? m_decoder->signalKey(row.messageIndex, row.signalIndex)
                       : -1;
    }
}

int DbcFlatModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_rows.size();
}

QHash<int, QByteArray> DbcFlatModel::roleNames() const
{
    return {
        { KindRole,          QByteArrayLiteral("kind")          },
        { DepthRole,         QByteArrayLiteral("depth")         },
        { MessageIndexRole,  QByteArrayLiteral("messageIndex")  },
        { SignalIndexRole,   QByteArrayLiteral("signalIndex")   },
        { ArbTextRole,       QByteArrayLiteral("arbText")       },
        { FormatTextRole,    QByteArrayLiteral("formatText")    },
        { BitsTextRole,      QByteArrayLiteral("bitsText")      },
        { FrequencyTextRole, QByteArrayLiteral("frequencyText") },
        { NameRole,          QByteArrayLiteral("name")          },
        { DescriptionRole,   QByteArrayLiteral("description")   },
        { UnitRole,          QByteArrayLiteral("unit")          },
        { MinimumRole,       QByteArrayLiteral("minimum")       },
        { MaximumRole,       QByteArrayLiteral("maximum")       },
        { LiveValueRole,     QByteArrayLiteral("liveValue")     },
        { LiveTextRole,      QByteArrayLiteral("liveText")      },
        { LiveFractionRole,  QByteArrayLiteral("liveFraction")  },
        { ColorIndexRole,    QByteArrayLiteral("colorIndex")    },
        { ExpandedRole,      QByteArrayLiteral("expanded")      },
        { HasChildrenRole,   QByteArrayLiteral("hasChildren")   },
        { SignalCountRole,   QByteArrayLiteral("signalCount")   }
    };
}

QVariant DbcFlatModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size() || !m_source)
        return {};

    const Row &row = m_rows.at(index.row());
    DbcMessageObject *message = m_source->message(row.messageIndex);
    if (!message)
        return {};

    const bool isMessage = row.kind == MessageRow;
    DbcSignalObject *sig = isMessage ? nullptr
                                     : m_source->signalAt(row.messageIndex, row.signalIndex);
    if (!isMessage && !sig)
        return {};

    switch (role) {
    case KindRole:         return row.kind;
    case DepthRole:        return isMessage ? 0 : 1;
    case MessageIndexRole: return row.messageIndex;
    case SignalIndexRole:  return row.signalIndex;
    case ColorIndexRole:   return row.colorIndex;

    case ArbTextRole:      return hexId(message->canId(), message->extended());
    case FormatTextRole:   return message->extended() ? QStringLiteral("Extended")
                                                      : QStringLiteral("Standard");
    case BitsTextRole:
        // A message row states its length; a signal row states where it sits
        // and how long it is - "12 (10)" is start bit 12, ten bits wide.
        return isMessage ? QStringLiteral("DLC = %1").arg(message->dlc())
                         : QStringLiteral("%1 (%2)").arg(sig->startBit()).arg(sig->bitLength());

    case FrequencyTextRole: {
        const double hz = m_liveRates.value(frameKeyOf(message->canId(), message->extended()), 0.0);
        return QStringLiteral("%1 Hz").arg(hz, 0, 'f', 1);
    }

    case NameRole:        return isMessage ? message->name() : sig->name();
    case DescriptionRole: return isMessage ? message->comment() : sig->comment();
    case UnitRole:        return isMessage ? QString() : sig->unit();
    case MinimumRole:     return isMessage ? QVariant() : QVariant(sig->minimum());
    case MaximumRole:     return isMessage ? QVariant() : QVariant(sig->maximum());

    case LiveValueRole:
        if (isMessage || row.decoderKey < 0)
            return QVariant();
        return m_liveValues.contains(row.decoderKey)
             ? QVariant(m_liveValues.value(row.decoderKey))
             : QVariant();

    case LiveTextRole: {
        if (isMessage || row.decoderKey < 0 || !m_liveValues.contains(row.decoderKey))
            return QString();
        const double value = m_liveValues.value(row.decoderKey);
        // "32 (P)" - the same string the Raw Analyzer shows for the same
        // signal, because both go through ValueFormat.h. Showing the name
        // ALONE was the earlier behaviour and was worse: it hid the code, so a
        // value that had drifted onto a neighbouring enum entry looked
        // identical to the correct one.
        return sig->formattedValue(value);
    }

    case LiveFractionRole: {
        if (isMessage || row.decoderKey < 0 || !m_liveValues.contains(row.decoderKey))
            return QVariant();
        const double lo = sig->minimum();
        const double hi = sig->maximum();
        if (!(hi > lo))
            return QVariant();          // no usable range - the bar stays hidden
        const double t = (m_liveValues.value(row.decoderKey) - lo) / (hi - lo);
        return qBound(0.0, t, 1.0);
    }

    case ExpandedRole:    return isMessage ? message->expanded() : false;
    case HasChildrenRole: return isMessage && message->signalList()
                               && message->signalList()->count() > 0;
    case SignalCountRole: return isMessage && message->signalList()
                               ? message->signalList()->count() : 0;
    default:
        return {};
    }
}

// ----------------------------------------------------------------- lookups

int DbcFlatModel::rowForMessage(int messageIndex) const
{
    for (int i = 0; i < m_rows.size(); ++i) {
        const Row &row = m_rows.at(i);
        if (row.kind == MessageRow && row.messageIndex == messageIndex)
            return i;
    }
    return -1;
}

int DbcFlatModel::rowForSignal(int messageIndex, int signalIndex) const
{
    for (int i = 0; i < m_rows.size(); ++i) {
        const Row &row = m_rows.at(i);
        if (row.kind == SignalRow && row.messageIndex == messageIndex
            && row.signalIndex == signalIndex) {
            return i;
        }
    }
    return -1;
}

QVariant DbcFlatModel::liveValueAt(int messageIndex, int signalIndex) const
{
    if (!m_decoder)
        return {};
    const int key = m_decoder->signalKey(messageIndex, signalIndex);
    if (key < 0 || !m_liveValues.contains(key))
        return {};
    return m_liveValues.value(key);
}

void DbcFlatModel::toggleExpanded(int row)
{
    if (!m_source || row < 0 || row >= m_rows.size())
        return;
    const Row &target = m_rows.at(row);
    if (target.kind != MessageRow)
        return;
    m_source->toggleExpanded(target.messageIndex);
}

// -------------------------------------------------------------- live values

void DbcFlatModel::ingest(const QVector<CanFrame> &frames)
{
    // Called on the DEVICE thread. Two early-outs before any work: the column
    // is not on screen, or there is nothing to decode with.
    if (!m_liveEnabled || !m_decoder || frames.isEmpty())
        return;

    m_decodeScratch.clear();
    m_decoder->decodeBatch(frames, m_decodeScratch);

    QMutexLocker locker(&m_liveMutex);
    for (const CanFrame &frame : frames)
        ++m_pendingCounts[frameKeyOf(frame.id, frame.extended)];
    // Last value wins inside a window: the column shows the newest reading,
    // not an average, so it agrees with what a scope would show.
    for (const DecodedSample &sample : std::as_const(m_decodeScratch))
        m_pendingValues.insert(sample.key, sample.value);
}

void DbcFlatModel::flushLive()
{
    QHash<int, double> values;
    QHash<quint32, int> counts;
    {
        QMutexLocker locker(&m_liveMutex);
        if (m_pendingValues.isEmpty() && m_pendingCounts.isEmpty() && m_liveRates.isEmpty())
            return;
        values.swap(m_pendingValues);
        counts.swap(m_pendingCounts);
    }

    const qint64 elapsedNs = m_liveWindow.nsecsElapsed();
    m_liveWindow.restart();
    // Wall clock, not the nominal interval: a tick delayed by a repaint would
    // otherwise report a rate that is too high by exactly the delay.
    const double seconds = double(elapsedNs) / 1e9;

    bool changed = !values.isEmpty();

    for (auto it = values.constBegin(); it != values.constEnd(); ++it)
        m_liveValues.insert(it.key(), it.value());

    if (seconds > 0.0) {
        // Every id we have ever seen decays, not just the ones that arrived in
        // this window - otherwise a message that stops transmitting keeps
        // displaying its last rate forever.
        const auto keys = m_liveRates.keys();
        for (quint32 key : keys) {
            if (!counts.contains(key))
                counts.insert(key, 0);
        }
        for (auto it = counts.constBegin(); it != counts.constEnd(); ++it) {
            const double instant = double(it.value()) / seconds;
            const double previous = m_liveRates.value(it.key(), instant);
            const double smoothed = previous + RateSmoothing * (instant - previous);
            if (!qFuzzyIsNull(smoothed - previous) || !m_liveRates.contains(it.key()))
                changed = true;
            m_liveRates.insert(it.key(), smoothed);
        }
    }

    if (!changed || m_rows.isEmpty())
        return;

    // One range emit rather than one per row: QQmlDelegateModel intersects the
    // range with the delegates it has actually created, so the cost tracks
    // what is visible, not how big the database is.
    emit dataChanged(index(0), index(m_rows.size() - 1),
                     { LiveValueRole, LiveTextRole, LiveFractionRole, FrequencyTextRole });
}
