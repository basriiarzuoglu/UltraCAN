#include "CanAnalyzerModel.h"

#include "dbc/ValueFormat.h"

#include <QMutexLocker>
#include <QTimer>

#include <algorithm>
#include <cmath>

#include <QFileInfo>

#include "dbc/CanDecoder.h"
#include "dbc/DbcParser.h"
#include "models/DbcModel.h"

namespace {

/*! Same key the decoder uses: bit 31 marks an extended identifier, so a
    standard 0x100 and an extended 0x100 are two different rows. */
inline quint32 frameKey(quint32 id, bool extended)
{
    return extended ? (id | 0x80000000u) : id;
}

/*! Index of the lowest set bit, then clear it. Iterating set bits instead of
    all 64 is what makes the per-frame cost proportional to how much actually
    moved - for most messages one or two bits. */
inline int takeLowestBit(quint64 &mask)
{
    const int index = qCountTrailingZeroBits(mask);
    mask &= mask - 1;
    return index;
}

}   // namespace

CanAnalyzerModel *CanAnalyzerModel::s_instance = nullptr;

CanAnalyzerModel::CanAnalyzerModel(QObject *parent)
    : QAbstractListModel(parent)
{
    if (!s_instance) {
        s_instance = this;
    } else {
        qCritical("CanAnalyzerModel: a SECOND instance was created (%p; first was %p).",
                  static_cast<void *>(this), static_cast<void *>(s_instance));
    }

    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setInterval(RefreshMs);
    m_refreshTimer->setTimerType(Qt::CoarseTimer);
    connect(m_refreshTimer, &QTimer::timeout, this, &CanAnalyzerModel::refresh);
    m_refreshTimer->start();
}

CanAnalyzerModel::~CanAnalyzerModel()
{
    if (s_instance == this)
        s_instance = nullptr;
}

CanAnalyzerModel *CanAnalyzerModel::instance()
{
    return s_instance;
}

// ================================================================ producer
//
// CAN device thread. One lock for the whole batch, no allocation per frame,
// and no contact with the model API - see the threading note in the header.

void CanAnalyzerModel::ingest(const CanFrame &frame)
{
    ingest(QVector<CanFrame> { frame });
}

void CanAnalyzerModel::ingest(const QVector<CanFrame> &frames)
{
    if (frames.isEmpty() || !m_running.load(std::memory_order_relaxed))
        return;

    QMutexLocker locker(&m_mutex);

    for (const CanFrame &frame : frames) {
        const quint32 key = frameKey(frame.id, frame.extended);

        int index = m_indexByKey.value(key, -1);
        if (index < 0) {
            index = int(m_entries.size());
            m_indexByKey.insert(key, index);
            m_entries.emplace_back();
            Entry &fresh = m_entries.back();
            fresh.id = frame.id;
            fresh.extended = frame.extended;
        }
        Entry &entry = m_entries[size_t(index)];

        entry.dlc = frame.dlc;
        entry.channel = frame.channel;
        entry.direction = frame.direction;
        entry.lastSeenNs = frame.timestampNs;
        ++entry.totalFrames;
        if (!frame.name.isEmpty())
            entry.name = frame.name;

        // Compare against the previous payload of THIS id. The very first frame
        // establishes the baseline and is not itself a change.
        const int bytes = qMin(int(frame.dlc), AnalysedBytes);
        Sample sample;
        sample.timestampNs = frame.timestampNs;

        if (entry.hasPrevious) {
            for (int b = 0; b < bytes; ++b) {
                const quint8 current = frame.payload[size_t(b)];
                const quint8 previous = entry.lastPayload[size_t(b)];
                if (current == previous)
                    continue;
                sample.toggleMask |= quint64(quint8(current ^ previous)) << (b * 8);
                sample.delta[size_t(b)] = quint8(current > previous ? current - previous
                                                                    : previous - current);
            }
        }

        for (int b = 0; b < AnalysedBytes; ++b)
            entry.lastPayload[size_t(b)] = b < bytes ? frame.payload[size_t(b)] : quint8(0);
        entry.hasPrevious = true;

        // -- push, adjusting the counters by hand
        if (sample.toggleMask != 0)
            ++entry.changedCount;
        quint64 walk = sample.toggleMask;
        while (walk)
            ++entry.toggleCount[size_t(takeLowestBit(walk))];
        for (int b = 0; b < AnalysedBytes; ++b)
            entry.deltaSum[size_t(b)] += sample.delta[size_t(b)];

        entry.window.push_back(sample);

        // -- age out: by time first, then by the hard cap
        const qint64 cutoff = frame.timestampNs - WindowNs;
        while (!entry.window.empty()
               && (entry.window.front().timestampNs < cutoff
                   || int(entry.window.size()) > MaxSamples)) {
            const Sample &old = entry.window.front();
            if (old.toggleMask != 0)
                --entry.changedCount;
            quint64 gone = old.toggleMask;
            while (gone)
                --entry.toggleCount[size_t(takeLowestBit(gone))];
            for (int b = 0; b < AnalysedBytes; ++b)
                entry.deltaSum[size_t(b)] -= old.delta[size_t(b)];
            entry.window.pop_front();
        }
    }
}

// ================================================================== refresh
//
// GUI thread, 10 Hz. Snapshots the accumulators into the display half, then
// emits ONE dataChanged for the whole table - the strip painter listens to
// refreshed() instead, so a repaint is one signal, not one per cell.

void CanAnalyzerModel::refresh()
{
    if (!m_running.load(std::memory_order_relaxed))
        return;

    size_t entryCount = 0;
    qint64 nowNs = 0;
    {
        QMutexLocker locker(&m_mutex);
        entryCount = m_entries.size();
        // "Now" is the newest timestamp any id has seen, not a wall clock: the
        // window must age out against the same bus clock the frames carry, or a
        // paused simulator would look like a live-but-silent bus.
        for (const Entry &entry : m_entries)
            nowNs = qMax(nowNs, entry.lastSeenNs);

        for (Entry &entry : m_entries)
            recomputeEntry(entry, nowNs);
    }

    const bool grew = entryCount != m_order.size();
    if (grew) {
        beginResetModel();
        rebuildOrder();
        endResetModel();
        emit countChanged();
    } else {
        if (m_autoSort)
            applySort();
        if (!m_order.empty())
            emit dataChanged(index(0), index(int(m_order.size()) - 1));
    }

    // Outside the mutex on purpose: both of these read only the display half,
    // which belongs to this thread, and the decoder takes its own read lock.
    // Holding ours across them would stall the device thread for no reason.
    decodeExpandedRows();

    ++m_refreshTick;
    emit refreshed();
}

void CanAnalyzerModel::resolveName(Entry &entry)
{
    if (entry.nameResolved)
        return;

    entry.dbcName.clear();
    entry.decoder = nullptr;
    entry.databaseIndex = -1;
    entry.known = false;

    // First match wins, in load order. messageNameForFrame() returns an empty
    // string for an id a database does not carry, which is exactly the
    // discriminator needed - no extra lookup table.
    for (size_t i = 0; i < m_databases.size(); ++i) {
        const CanDecoder *decoder = m_databases[i].decoder.get();
        if (!decoder)
            continue;
        const QString name = decoder->messageNameForFrame(entry.id, entry.extended);
        if (name.isEmpty())
            continue;

        entry.dbcName = name;
        entry.decoder = decoder;
        entry.databaseIndex = int(i);
        entry.known = true;
        break;
    }

    entry.nameResolved = true;
}

void CanAnalyzerModel::invalidateResolution()
{
    // Every entry's decoder pointer may now dangle and every signal key it
    // holds belongs to a database that may be gone. Drop the lot rather than
    // trying to work out which survived - the re-resolve is a hash lookup per
    // id and happens once.
    for (Entry &entry : m_entries) {
        entry.nameResolved = false;
        entry.decoder = nullptr;
        entry.databaseIndex = -1;
        entry.known = false;
        entry.dbcName.clear();
        entry.signalKeys.clear();
        entry.signalValues.clear();
        resolveName(entry);
    }

    // Loading or dropping a database changes `known`, which is the primary
    // key of the default id sort - so the row order is stale the moment this
    // loop ends. Re-sorting here rather than at the three call sites keeps the
    // rule in one place: whoever invalidates resolution gets the reorder for
    // free. Cheap by construction (a stable_sort over an int vector), and the
    // callers already emit dataChanged over the full range afterwards, which
    // is what makes the reindexed rows repaint. No-ops when a non-id sort is
    // active, since that branch never looks at `known`.
    applySort();
}

void CanAnalyzerModel::decodeExpandedRows()
{
    if (m_expandedCount <= 0 || m_databases.empty())
        return;

    // Reused across refreshes so an open row does not allocate ten times a
    // second for the rest of the session.
    thread_local QVector<DecodedSample> samples;

    // Iterate through m_order, never with a range-for over m_entries. A deque
    // keeps REFERENCES valid across push_back but not ITERATORS, and the device
    // thread appends entries while this runs. m_order is owned by this thread
    // and every index in it addresses an element that already exists, so
    // indexing is safe where iterating is not.
    for (int entryIndex : m_order) {
        Entry &entry = m_entries[size_t(entryIndex)];
        if (!entry.expanded)
            continue;
        // Each row decodes with the database that claimed it, not with a
        // global one - two rows on screen can be resolving against two
        // different files.
        if (!entry.decoder) {
            entry.signalKeys.clear();
            entry.signalValues.clear();
            continue;
        }

        // A synthetic frame carrying the last payload we saw. CanDecoder wants
        // a CanFrame and we have every field it reads, so this is the existing
        // decode path applied to stored bytes rather than a live one.
        CanFrame probe;
        probe.id = entry.id;
        probe.extended = entry.extended;
        probe.dlc = entry.dlc;
        probe.timestampNs = entry.lastSeenNs;
        for (int b = 0; b < AnalysedBytes; ++b)
            probe.payload[size_t(b)] = entry.displayPayload[size_t(b)];

        samples.clear();
        entry.decoder->decode(probe, samples);

        const int count = int(samples.size());
        if (entry.signalKeys.size() != count) {
            entry.signalKeys.resize(count);
            entry.signalValues.resize(count);
        }
        for (int i = 0; i < count; ++i) {
            entry.signalKeys[i] = samples[i].key;
            entry.signalValues[i] = samples[i].value;
        }
    }
}

void CanAnalyzerModel::recomputeEntry(Entry &entry, qint64 nowNs)
{
    // Prune here too. A message that stopped arriving never gets pruned by
    // ingest(), and a row frozen at "9.3 Hz, 70% change" for a node that was
    // unplugged five minutes ago is a lie the whole view rests on.
    const qint64 cutoff = nowNs - WindowNs;
    while (!entry.window.empty() && entry.window.front().timestampNs < cutoff) {
        const Sample &old = entry.window.front();
        if (old.toggleMask != 0)
            --entry.changedCount;
        quint64 gone = old.toggleMask;
        while (gone)
            --entry.toggleCount[size_t(takeLowestBit(gone))];
        for (int b = 0; b < AnalysedBytes; ++b)
            entry.deltaSum[size_t(b)] -= old.delta[size_t(b)];
        entry.window.pop_front();
    }

    const int samples = int(entry.window.size());

    if (samples <= 0) {
        entry.frequency = 0.0;
        entry.rawChange = 0.0;
        entry.rawBitActivity.fill(0.0f);
        entry.rawByteActivity.fill(0.0f);
    } else {
        // Frequency comes from the span the samples ACTUALLY cover, not from
        // the nominal 2 s. Two cases make the difference visible:
        //   - MaxSamples truncates the window for a fast message, so dividing
        //     by 2 s reported a 1 kHz message as 256 Hz - the cap silently
        //     became the answer;
        //   - a message that just appeared has only covered a fraction of the
        //     window, and would otherwise ramp up to its true rate over two
        //     seconds instead of reading correctly from the third frame.
        const qint64 spanNs = entry.window.back().timestampNs
                              - entry.window.front().timestampNs;
        entry.frequency = (samples >= 2 && spanNs > 0)
                          ? double(samples - 1) * 1e9 / double(spanNs)
                          : double(samples) / (double(WindowNs) / 1e9);

        // The Change denominator stays the sample COUNT: "of the frames I saw,
        // this fraction differed from the one before".
        entry.rawChange = 100.0 * double(entry.changedCount) / double(samples);

        const double inverse = 1.0 / double(samples);
        for (int b = 0; b < AnalysedBits; ++b)
            entry.rawBitActivity[size_t(b)] = float(double(entry.toggleCount[size_t(b)]) * inverse);
        // Normalised against a full-scale swing, so "this byte moves by 128 on
        // every frame" is 0.5 and a jittering LSB is nearly nothing.
        for (int b = 0; b < AnalysedBytes; ++b)
            entry.rawByteActivity[size_t(b)] =
                float(double(entry.deltaSum[size_t(b)]) * inverse / 255.0);
    }

    entry.change = qMax(0.0, entry.rawChange - entry.referenceChange);
    entry.displayPayload = entry.lastPayload;
    resolveName(entry);
    buildCells(entry);
}

void CanAnalyzerModel::buildCells(Entry &entry) const
{
    entry.cellActivity.fill(0.0f);
    entry.cellValues.fill(0);

    if (m_analysisType == ByteValueChange) {
        for (int cell = 0; cell < AnalysedBytes; ++cell) {
            const int byteIndex = (m_byteOrder == Motorola) ? (AnalysedBytes - 1 - cell) : cell;
            const float raw = entry.rawByteActivity[size_t(byteIndex)];
            const float reference = entry.refByteActivity[size_t(byteIndex)];
            entry.cellActivity[size_t(cell)] = qBound(0.0f, raw - reference, 1.0f);
            entry.cellValues[size_t(cell)] = entry.displayPayload[size_t(byteIndex)];
        }
        return;
    }

    for (int cell = 0; cell < AnalysedBits; ++cell) {
        const int bit = bitForCell(cell);
        const float raw = entry.rawBitActivity[size_t(bit)];
        const float reference = entry.refBitActivity[size_t(bit)];
        entry.cellActivity[size_t(cell)] = qBound(0.0f, raw - reference, 1.0f);
        const int byteIndex = bit / 8;
        const int bitInByte = bit % 8;
        entry.cellValues[size_t(cell)] =
            quint8((entry.displayPayload[size_t(byteIndex)] >> bitInByte) & 0x01);
    }
}

int CanAnalyzerModel::bitForCell(int cell) const
{
    // Cells run left to right. Within a byte the MSB is drawn first, which is
    // how a hex byte reads; Motorola additionally reverses the byte order, so
    // a multi-byte big-endian value appears as one contiguous run.
    const int cellByte = cell / 8;
    const int byteIndex = (m_byteOrder == Motorola) ? (AnalysedBytes - 1 - cellByte) : cellByte;
    const int bitInByte = 7 - (cell % 8);
    return byteIndex * 8 + bitInByte;
}

// ============================================================ model access

int CanAnalyzerModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_order.size());
}

const CanAnalyzerModel::Entry *CanAnalyzerModel::entryForRow(int row) const
{
    if (row < 0 || row >= int(m_order.size()))
        return nullptr;
    return &m_entries[size_t(m_order[size_t(row)])];
}

CanAnalyzerModel::Entry *CanAnalyzerModel::entryForRow(int row)
{
    return const_cast<Entry *>(std::as_const(*this).entryForRow(row));
}

QVariant CanAnalyzerModel::data(const QModelIndex &index, int role) const
{
    const Entry *entry = entryForRow(index.row());
    if (!entry)
        return {};

    switch (role) {
    case ArbIdRole:     return entry->id;
    case IdTextRole:    return QStringLiteral("%1h")
                               .arg(entry->id, entry->extended ? 8 : 3, 16, QLatin1Char('0'))
                               .toUpper();
    case ExtendedRole:  return entry->extended;
    case DlcRole:       return int(entry->dlc);
    case ChannelRole:   return m_channelNames.value(entry->channel, QStringLiteral("CAN?"));
    case DirectionRole: return entry->direction == CanFrame::Tx ? QStringLiteral("Tx")
                                                                : QStringLiteral("Rx");
    case FrequencyRole: return entry->frequency;
    case ChangeRole:    return entry->change;
    case NameRole:      return entry->dbcName.isEmpty() ? entry->name : entry->dbcName;
    case ArbTextRole: {
        const QString hex = QStringLiteral("0x%1")
                            .arg(entry->id, entry->extended ? 8 : 3, 16, QLatin1Char('0'))
                            .toUpper();
        return entry->known ? QStringLiteral("%1 (%2)").arg(entry->dbcName, hex) : hex;
    }
    case KnownRole:        return entry->known;
    case ExpandedRole:     return entry->expanded;
    case SignalCountRole:  return entry->signalKeys.size();
    // An index into Theme.seriesPalette, not a colour: the C++ side must not
    // know what the palette looks like.
    case ColorRole:     return int(m_order[size_t(index.row())] % 8);
    case SeenRole:      return double(entry->totalFrames);
    default:            return {};
    }
}

QHash<int, QByteArray> CanAnalyzerModel::roleNames() const
{
    return {
        { ArbIdRole,     QByteArrayLiteral("arbId")     },
        { IdTextRole,    QByteArrayLiteral("idText")    },
        { ExtendedRole,  QByteArrayLiteral("extended")  },
        { DlcRole,       QByteArrayLiteral("dlc")       },
        { ChannelRole,   QByteArrayLiteral("channel")   },
        { DirectionRole, QByteArrayLiteral("direction") },
        { FrequencyRole, QByteArrayLiteral("frequency") },
        { ChangeRole,    QByteArrayLiteral("change")    },
        { NameRole,        QByteArrayLiteral("name")        },
        { ArbTextRole,     QByteArrayLiteral("arbText")     },
        { KnownRole,       QByteArrayLiteral("known")       },
        { ExpandedRole,    QByteArrayLiteral("expanded")    },
        { SignalCountRole, QByteArrayLiteral("signalCount") },
        { ColorRole,     QByteArrayLiteral("colorIndex")},
        { SeenRole,      QByteArrayLiteral("seen")      }
    };
}

QVariantMap CanAnalyzerModel::get(int row) const
{
    QVariantMap map;
    const QHash<int, QByteArray> names = roleNames();
    for (auto it = names.cbegin(); it != names.cend(); ++it)
        map.insert(QString::fromLatin1(it.value()), data(index(row), it.key()));
    return map;
}

const float *CanAnalyzerModel::cellActivity(int row) const
{
    const Entry *entry = entryForRow(row);
    return entry ? entry->cellActivity.data() : nullptr;
}

const quint8 *CanAnalyzerModel::cellValues(int row) const
{
    const Entry *entry = entryForRow(row);
    return entry ? entry->cellValues.data() : nullptr;
}

// ================================================================== sorting
//
// Sorting REINDEXES; it never moves rows. beginMoveRows at 10 Hz would make
// the view unusable - the scroll position fights the data and a delegate the
// user is pointing at slides out from under the cursor. Reindexing keeps the
// delegates where they are and swaps their content, which is what a live
// heatmap should do anyway.

void CanAnalyzerModel::rebuildOrder()
{
    m_order.resize(m_entries.size());
    for (size_t i = 0; i < m_order.size(); ++i)
        m_order[i] = int(i);
    applySort();
}

void CanAnalyzerModel::applySort()
{
    if (m_order.empty())
        return;

    const auto key = [this](int entryIndex) -> double {
        const Entry &entry = m_entries[size_t(entryIndex)];
        switch (m_sortColumn) {
        case SortByFrequency: return entry.frequency;
        case SortByChange:    return entry.change;
        default:              return 0.0;
        }
    };

    if (m_sortColumn == SortById) {
        // Insertion order is arrival order, which is not id order. Sort by the
        // identifier itself so "unsorted" is stable and readable.
        std::stable_sort(m_order.begin(), m_order.end(), [this](int a, int b) {
            const Entry &ea = m_entries[size_t(a)];
            const Entry &eb = m_entries[size_t(b)];
            // Tier 1 - decoded traffic first. `known` is the authority here,
            // not "does the name differ from the hex string": an entry can
            // carry a name off the frame itself (Entry::name) without any
            // database claiming it, and a database can claim an id whose
            // message name we would render identically to its hex. Only
            // resolveName() sets `known`, and only against a loaded database.
            if (ea.known != eb.known)
                return ea.known;
            // Tier 2 - ascending numeric id within each tier. Standard ids
            // stay ahead of extended ones so 0x7FF does not sort after an
            // extended 0x001; the two id spaces are disjoint, not one range.
            if (ea.extended != eb.extended)
                return !ea.extended;
            return ea.id < eb.id;
        });
        return;
    }

    std::stable_sort(m_order.begin(), m_order.end(), [&](int a, int b) {
        const double ka = key(a);
        const double kb = key(b);
        if (qFuzzyCompare(ka + 1.0, kb + 1.0)) {
            // Ties keep a deterministic order, or rows shuffle every refresh.
            return m_entries[size_t(a)].id < m_entries[size_t(b)].id;
        }
        return m_sortDescending ? (ka > kb) : (ka < kb);
    });
}

void CanAnalyzerModel::sortBy(int column, bool descending)
{
    m_sortColumn = column;
    m_sortDescending = descending;
    applySort();
    if (!m_order.empty())
        emit dataChanged(index(0), index(int(m_order.size()) - 1));
    emit sortChanged();
    emit refreshed();
}

void CanAnalyzerModel::unsort()
{
    sortBy(SortById, false);
}

void CanAnalyzerModel::setAutoSort(bool enabled)
{
    if (m_autoSort == enabled)
        return;
    m_autoSort = enabled;
    if (enabled && m_sortColumn == SortById)
        m_sortColumn = SortByChange;    // "keep the busiest on top" is the point
    emit autoSortChanged();
    emit sortChanged();
}

// ================================================================ reference

void CanAnalyzerModel::setReference()
{
    QMutexLocker locker(&m_mutex);
    for (Entry &entry : m_entries) {
        entry.refBitActivity = entry.rawBitActivity;
        entry.refByteActivity = entry.rawByteActivity;
        entry.referenceChange = entry.rawChange;
        entry.change = 0.0;
        buildCells(entry);
    }
    locker.unlock();

    m_referenceSet = true;
    emit referenceChanged();
    if (!m_order.empty())
        emit dataChanged(index(0), index(int(m_order.size()) - 1));
    emit refreshed();
    setLastMessage(tr("Reference captured - the table now shows only NEW activity."));
}

void CanAnalyzerModel::clearReference()
{
    QMutexLocker locker(&m_mutex);
    for (Entry &entry : m_entries) {
        entry.refBitActivity.fill(0.0f);
        entry.refByteActivity.fill(0.0f);
        entry.referenceChange = 0.0;
        entry.change = entry.rawChange;
        buildCells(entry);
    }
    locker.unlock();

    m_referenceSet = false;
    emit referenceChanged();
    if (!m_order.empty())
        emit dataChanged(index(0), index(int(m_order.size()) - 1));
    emit refreshed();
    setLastMessage(tr("Reference cleared - showing absolute activity again."));
}

void CanAnalyzerModel::reset()
{
    beginResetModel();
    {
        QMutexLocker locker(&m_mutex);
        m_entries.clear();
        m_indexByKey.clear();
    }
    m_order.clear();
    m_referenceSet = false;
    clearSelection();
    endResetModel();

    emit countChanged();
    emit referenceChanged();
    emit refreshed();
    setLastMessage(tr("Analyzer cleared."));
}

void CanAnalyzerModel::setRunning(bool running)
{
    if (m_running.load(std::memory_order_relaxed) == running)
        return;
    m_running.store(running, std::memory_order_relaxed);
    emit runningChanged();
    setLastMessage(running ? tr("Analysis resumed.") : tr("Analysis paused."));
}

// ============================================================ display state

void CanAnalyzerModel::setAnalysisType(int type)
{
    if (m_analysisType == type)
        return;
    m_analysisType = type;

    QMutexLocker locker(&m_mutex);
    for (Entry &entry : m_entries)
        buildCells(entry);
    locker.unlock();

    // Cell geometry changed, so the selection no longer means what it meant.
    clearSelection();
    emit analysisTypeChanged();
    emit refreshed();
}

void CanAnalyzerModel::setByteOrder(int order)
{
    if (m_byteOrder == order)
        return;
    m_byteOrder = order;

    QMutexLocker locker(&m_mutex);
    for (Entry &entry : m_entries)
        buildCells(entry);
    locker.unlock();

    clearSelection();
    emit byteOrderChanged();
    emit refreshed();
}

QStringList CanAnalyzerModel::analysisTypeNames() const
{
    return { tr("Bit changing frequency"), tr("Byte value change") };
}

QStringList CanAnalyzerModel::byteOrderNames() const
{
    return { tr("Intel"), tr("Motorola") };
}

void CanAnalyzerModel::setChannelTarget(DbcModel *model)
{
    m_channelTarget = model;
}

void CanAnalyzerModel::refreshNames()
{
    QMutexLocker locker(&m_mutex);
    invalidateResolution();
    locker.unlock();

    // An expanded panel is showing the OLD database's signal names right now;
    // decode again before telling anyone, so the rebuild lands on real values.
    decodeExpandedRows();

    if (!m_order.empty())
        emit dataChanged(index(0), index(int(m_order.size()) - 1));
    emit namesChanged();
    emit refreshed();
}

void CanAnalyzerModel::setChannelNames(const QStringList &names)
{
    m_channelNames = names;
}

void CanAnalyzerModel::setLastMessage(const QString &message)
{
    if (m_lastMessage == message)
        return;
    m_lastMessage = message;
    emit lastMessageChanged();
}

// ============================================================== selection
//
// Pick two cells, get a channel. This is the payoff of the whole view: a lit
// run of bits is a field, and naming it turns it into something the Graph Plot
// can draw.

int CanAnalyzerModel::selectionRow() const
{
    if (m_selectionEntry < 0)
        return -1;
    const auto it = std::find(m_order.begin(), m_order.end(), m_selectionEntry);
    return it == m_order.end() ? -1 : int(std::distance(m_order.begin(), it));
}

void CanAnalyzerModel::selectFirstBit(int row, int cell)
{
    if (!rowIsValid(row) || cell < 0 || cell >= cellCount())
        return;
    m_selectionEntry = m_order[size_t(row)];
    m_firstBit = cell;
    if (m_secondBit >= cellCount())
        m_secondBit = -1;
    updateSelectionSummary();
    emit selectionChanged();
    emit refreshed();
}

void CanAnalyzerModel::selectSecondBit(int row, int cell)
{
    if (!rowIsValid(row) || cell < 0 || cell >= cellCount())
        return;
    const int entryIndex = m_order[size_t(row)];
    if (m_selectionEntry >= 0 && m_selectionEntry != entryIndex) {
        // A channel cannot span two messages. Silently re-anchoring the first
        // bit to a different message would be worse than saying so.
        setLastMessage(tr("Both bits must belong to the same message - "
                          "the first bit was cleared."));
        m_firstBit = -1;
    }
    m_selectionEntry = entryIndex;
    m_secondBit = cell;
    updateSelectionSummary();
    emit selectionChanged();
    emit refreshed();
}

void CanAnalyzerModel::clearSelection()
{
    if (m_selectionEntry < 0 && m_firstBit < 0 && m_secondBit < 0)
        return;
    m_selectionEntry = -1;
    m_firstBit = -1;
    m_secondBit = -1;
    m_selectionSummary.clear();
    emit selectionChanged();
    emit refreshed();
}

void CanAnalyzerModel::updateSelectionSummary()
{
    if (!canCreateChannel()) {
        m_selectionSummary.clear();
        return;
    }

    const int lowCell = qMin(m_firstBit, m_secondBit);
    const int highCell = qMax(m_firstBit, m_secondBit);

    if (m_analysisType == ByteValueChange) {
        const int bytes = highCell - lowCell + 1;
        m_selectionSummary = tr("bytes %1..%2  (%3 bit%4)")
                             .arg(lowCell).arg(highCell)
                             .arg(bytes * 8).arg(bytes * 8 == 1 ? QString() : QStringLiteral("s"));
        return;
    }

    const int length = highCell - lowCell + 1;
    const int startBit = bitForCell(m_byteOrder == Motorola ? lowCell : highCell);
    m_selectionSummary = tr("start bit %1, length %2, %3")
                         .arg(startBit).arg(length)
                         .arg(m_byteOrder == Motorola ? tr("Motorola") : tr("Intel"));
}

QString CanAnalyzerModel::createChannelBetweenSelectedBits(const QString &name)
{
    if (!canCreateChannel()) {
        setLastMessage(tr("Select a first and a second bit on the same row."));
        return {};
    }
    if (!m_channelTarget) {
        setLastMessage(tr("No editor document is attached - the backend is not wired up."));
        return {};
    }

    const Entry *entry = &m_entries[size_t(m_selectionEntry)];

    const int lowCell = qMin(m_firstBit, m_secondBit);
    const int highCell = qMax(m_firstBit, m_secondBit);

    int startBit = 0;
    int bitLength = 0;
    if (m_analysisType == ByteValueChange) {
        const int lowByte = m_byteOrder == Motorola ? (AnalysedBytes - 1 - highCell) : lowCell;
        bitLength = (highCell - lowCell + 1) * 8;
        startBit = lowByte * 8;
    } else {
        bitLength = highCell - lowCell + 1;
        // Intel counts from the LSB of the field, Motorola from its MSB, which
        // is the leftmost drawn cell in each case.
        startBit = bitForCell(m_byteOrder == Motorola ? lowCell : highCell);
    }

    // Find or create the message for this identifier.
    int messageIndex = -1;
    for (int i = 0; i < m_channelTarget->messageCount(); ++i) {
        DbcMessageObject *candidate = m_channelTarget->message(i);
        if (candidate && candidate->canId() == entry->id
            && candidate->extended() == entry->extended) {
            messageIndex = i;
            break;
        }
    }

    bool createdMessage = false;
    if (messageIndex < 0) {
        messageIndex = m_channelTarget->addMessage();
        if (messageIndex < 0) {
            setLastMessage(tr("Could not add a message to the database."));
            return {};
        }
        createdMessage = true;
        m_channelTarget->setMessageProperty(messageIndex, QStringLiteral("canId"), entry->id);
        m_channelTarget->setMessageProperty(messageIndex, QStringLiteral("extended"), entry->extended);
        m_channelTarget->setMessageProperty(messageIndex, QStringLiteral("dlc"), int(entry->dlc));
        m_channelTarget->setMessageProperty(
            messageIndex, QStringLiteral("name"),
            entry->name.isEmpty()
                ? tr("MSG_%1").arg(entry->id, entry->extended ? 8 : 3, 16, QLatin1Char('0')).toUpper()
                : entry->name);
    }

    const int signalIndex = m_channelTarget->addSignal(messageIndex);
    if (signalIndex < 0) {
        setLastMessage(tr("Could not add a signal to the message."));
        return {};
    }

    const QString signalName = name.isEmpty()
        ? tr("NEW_%1_%2").arg(startBit).arg(bitLength)
        : name;

    m_channelTarget->setSignalProperty(messageIndex, signalIndex, QStringLiteral("name"), signalName);
    m_channelTarget->setSignalProperty(messageIndex, signalIndex, QStringLiteral("startBit"), startBit);
    m_channelTarget->setSignalProperty(messageIndex, signalIndex, QStringLiteral("bitLength"), bitLength);
    // DbcModel's convention, matching the .dbc file format: 0 = Motorola, 1 = Intel.
    m_channelTarget->setSignalProperty(messageIndex, signalIndex, QStringLiteral("byteOrder"),
                                  m_byteOrder == Motorola ? 0 : 1);
    m_channelTarget->setSignalProperty(messageIndex, signalIndex, QStringLiteral("factor"), 1.0);
    m_channelTarget->setSignalProperty(messageIndex, signalIndex, QStringLiteral("offset"), 0.0);
    m_channelTarget->setSignalProperty(messageIndex, signalIndex, QStringLiteral("minimum"), 0.0);
    m_channelTarget->setSignalProperty(messageIndex, signalIndex, QStringLiteral("maximum"),
                                  double((quint64(1) << qMin(bitLength, 53)) - 1));
    m_channelTarget->setSignalProperty(messageIndex, signalIndex, QStringLiteral("comment"),
                                  tr("Created from the raw data analyzer."));

    setLastMessage(createdMessage
        ? tr("Created %1 (start %2, length %3) on a new message.")
              .arg(signalName).arg(startBit).arg(bitLength)
        : tr("Created %1 (start %2, length %3).")
              .arg(signalName).arg(startBit).arg(bitLength));

    clearSelection();
    return signalName;
}

// ================================================================ expansion
//
// Stored on the entry, so it survives a re-sort - see the header note.

void CanAnalyzerModel::setExpanded(int row, bool expanded)
{
    if (!rowIsValid(row))
        return;

    Entry &entry = m_entries[size_t(m_order[size_t(row)])];
    if (entry.expanded == expanded)
        return;

    entry.expanded = expanded;
    m_expandedCount += expanded ? 1 : -1;

    if (!expanded) {
        entry.signalKeys.clear();
        entry.signalValues.clear();
    } else {
        // Decode immediately rather than leaving the panel blank until the
        // next tick - a hundred milliseconds of empty rows reads as "this
        // message has no signals".
        decodeExpandedRows();
    }

    // ONE row changed. No reset, so the view keeps its scroll position and
    // every other delegate keeps its state.
    emit dataChanged(index(row), index(row), { ExpandedRole, SignalCountRole });
    emit expandedChanged();
    emit refreshed();
}

void CanAnalyzerModel::toggleExpanded(int row)
{
    if (!rowIsValid(row))
        return;
    setExpanded(row, !m_entries[size_t(m_order[size_t(row)])].expanded);
}

void CanAnalyzerModel::collapseAll()
{
    if (m_expandedCount <= 0)
        return;

    for (int entryIndex : m_order) {       // see decodeExpandedRows()
        Entry &entry = m_entries[size_t(entryIndex)];
        entry.expanded = false;
        entry.signalKeys.clear();
        entry.signalValues.clear();
    }
    m_expandedCount = 0;

    if (!m_order.empty())
        emit dataChanged(index(0), index(int(m_order.size()) - 1),
                         { ExpandedRole, SignalCountRole });
    emit expandedChanged();
    emit refreshed();
}

QVariantList CanAnalyzerModel::signalDescriptors(int row) const
{
    QVariantList out;
    const Entry *entry = entryForRow(row);
    if (!entry || !entry->decoder)
        return out;

    // Keys are meaningful only to the decoder that issued them, so the lookups
    // go through the SAME one that produced entry->signalKeys.
    const CanDecoder *decoder = entry->decoder;
    out.reserve(entry->signalKeys.size());
    for (int i = 0; i < entry->signalKeys.size(); ++i) {
        const int key = entry->signalKeys[i];
        out.append(QVariantMap {
            { QStringLiteral("name"),    decoder->signalName(key) },
            { QStringLiteral("unit"),    decoder->unit(key) },
            { QStringLiteral("minimum"), decoder->minimum(key) },
            { QStringLiteral("maximum"), decoder->maximum(key) },
            { QStringLiteral("key"),     key }
        });
    }
    return out;
}

double CanAnalyzerModel::signalValue(int row, int signalIndex) const
{
    const Entry *entry = entryForRow(row);
    if (!entry || signalIndex < 0 || signalIndex >= entry->signalValues.size())
        return 0.0;
    return entry->signalValues[signalIndex];
}

QString CanAnalyzerModel::signalText(int row, int signalIndex) const
{
    const Entry *entry = entryForRow(row);
    if (!entry || signalIndex < 0 || signalIndex >= entry->signalValues.size())
        return {};

    const double value = entry->signalValues[signalIndex];
    const int key = entry->signalKeys.value(signalIndex, -1);

    if (!entry->decoder)
        return QString::number(value, 'f', UltraCAN::ValueDecimals);

    // "32 (P)" for a value the signal's VAL_ table names, otherwise three
    // decimals with the trailing zeros kept - a column of readouts that jumps
    // between "3500" and "3500.125" as values cross an integer is unreadable
    // when you are scanning for the one that moved. The rule itself is in
    // ValueFormat.h so the DBC Editor shows the identical string.
    const QString text = entry->decoder->formatValue(key, value);

    // A named enumeration member has no unit. Appending the DBC's unit field
    // to it produces "32 (P) -", which reads as a third column that is not
    // there; most .dbc files put a bare "-" in the unit of an enum for exactly
    // the reason that there is nothing to put.
    if (entry->decoder->isNamedValue(key, value))
        return text;

    const QString unit = entry->decoder->unit(key);
    return unit.isEmpty() ? text : QStringLiteral("%1 %2").arg(text, unit);
}

// ====================================================== database management
//
// The analyzer's databases are ITS OWN. Nothing here touches the DBC Editor's
// document, and nothing the Editor does reaches in here: the point of the
// feature is that you can throw three candidate .dbc files at an unknown bus
// without disturbing the file you are actually authoring.

QVariantList CanAnalyzerModel::loadedDbcs() const
{
    QVariantList out;
    out.reserve(int(m_databases.size()));
    for (const AnalyzerDatabase &database : m_databases) {
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

bool CanAnalyzerModel::loadAnalyzerDbc(const QUrl &url)
{
    const QString path = url.isLocalFile() ? url.toLocalFile() : url.toString();
    if (path.isEmpty()) {
        setLastMessage(tr("No file was given."));
        return false;
    }

    // Parse BEFORE touching the list. A file that will not parse must cost the
    // user nothing - not their existing databases, and not a half-added row.
    QString error;
    const DbcDatabase parsed = DbcParser::parseFile(path, &error);

    if (!error.isEmpty()) {
        setLastMessage(error);
        return false;
    }
    if (parsed.isEmpty()) {
        setLastMessage(tr("%1 contains no messages - is it a .dbc file?")
                       .arg(QFileInfo(path).fileName()));
        return false;
    }

    const QString fileName = QFileInfo(path).fileName();
    for (const AnalyzerDatabase &existing : m_databases) {
        if (existing.path == path) {
            setLastMessage(tr("%1 is already loaded.").arg(fileName));
            return false;
        }
    }

    AnalyzerDatabase database;
    database.name = parsed.name.isEmpty() ? fileName : parsed.name;
    database.path = path;
    database.url = url;
    database.messageCount = int(parsed.messages.size());
    for (const DbcMessageData &message : parsed.messages)
        database.signalCount += int(message.signalList.size());

    // Its own decoder, holding its own compiled table. That is what lets two
    // databases with overlapping ids coexist: they never share a lookup.
    database.decoder = std::make_unique<CanDecoder>();
    database.decoder->setDatabase(parsed);

    {
        QMutexLocker locker(&m_mutex);
        m_databases.push_back(std::move(database));
        invalidateResolution();
    }

    decodeExpandedRows();
    if (!m_order.empty())
        emit dataChanged(index(0), index(int(m_order.size()) - 1));
    emit databasesChanged();
    emit namesChanged();
    emit refreshed();

    setLastMessage(tr("Loaded %1 - %2 message(s), %3 signal(s).")
                   .arg(fileName)
                   .arg(m_databases.back().messageCount)
                   .arg(m_databases.back().signalCount));
    qInfo("Analyzer DBC: +%s (%d messages, %d signals); %lld loaded",
          qPrintable(fileName), m_databases.back().messageCount,
          m_databases.back().signalCount, qint64(m_databases.size()));
    return true;
}

void CanAnalyzerModel::removeAnalyzerDbc(int dbcIndex)
{
    if (dbcIndex < 0 || dbcIndex >= int(m_databases.size()))
        return;

    const QString name = m_databases[size_t(dbcIndex)].name;

    {
        QMutexLocker locker(&m_mutex);
        // Erase first, THEN re-resolve. Every entry holds a raw pointer to a
        // decoder owned by this vector, and one of those pointers is about to
        // dangle - invalidateResolution() is what clears them, so it has to run
        // against the list as it will be, not as it was.
        m_databases.erase(m_databases.begin() + dbcIndex);
        invalidateResolution();
    }

    decodeExpandedRows();
    if (!m_order.empty())
        emit dataChanged(index(0), index(int(m_order.size()) - 1));
    emit databasesChanged();
    emit namesChanged();
    emit refreshed();

    setLastMessage(tr("Removed %1 - its IDs are raw again.").arg(name));
}

void CanAnalyzerModel::clearAnalyzerDbcs()
{
    if (m_databases.empty())
        return;

    {
        QMutexLocker locker(&m_mutex);
        m_databases.clear();
        invalidateResolution();
    }

    decodeExpandedRows();
    if (!m_order.empty())
        emit dataChanged(index(0), index(int(m_order.size()) - 1));
    emit databasesChanged();
    emit namesChanged();
    emit refreshed();

    setLastMessage(tr("All databases removed - showing raw IDs."));
}

int CanAnalyzerModel::matchedIdCount(int dbcIndex) const
{
    if (dbcIndex < 0 || dbcIndex >= int(m_databases.size()))
        return 0;

    int matched = 0;
    for (int entryIndex : m_order) {            // see decodeExpandedRows()
        if (m_entries[size_t(entryIndex)].databaseIndex == dbcIndex)
            ++matched;
    }
    return matched;
}
