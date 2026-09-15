#pragma once

#include <QAbstractListModel>
#include <QElapsedTimer>
#include <QHash>
#include <QMutex>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QUrl>
#include <QtQml/qqmlregistration.h>

#include <array>
#include <atomic>
#include <deque>
#include <memory>
#include <utility>
#include <vector>

#include "core/CanFrame.h"
#include "dbc/CanDecoder.h"

class QTimer;
class DbcModel;

/*!
    \brief Live bit/byte activity analyzer for reverse-engineering an
           undocumented bus.

    One row per arbitration id. For each id it answers the question a reverse
    engineer actually asks: \e {which bits move, and how much, right now}. Turn
    the steering wheel and the bits carrying the wheel angle light up.

    \section1 The scoring window

    Everything - frequency, the Change score and the per-bit heat - comes out of
    ONE sliding window (\l WindowNs, 2 s), which is what keeps the three numbers
    consistent with each other:

    \list
    \li \b frequency = samples / 2 s
    \li \b Change    = 100 x (samples whose payload differed from the previous
                       one) / samples. A message repeating a constant payload
                       scores 0 no matter how fast it repeats.
    \li \b {bit heat} = toggles of that bit / samples, i.e. "this bit flipped in
                       70% of the frames I saw".
    \endlist

    \section1 Why the counters are incremental

    The naive implementation recomputes the window on every refresh: 512
    samples x 64 bits x 100 ids x 10 Hz is 32 million operations a second for a
    display that changes ten times a second. Instead every counter is adjusted
    when a sample is pushed and when it ages out, iterating only the bits that
    actually toggled (\c popcount, typically one or two). The refresh is then a
    pure copy, O(ids), and the per-frame cost is independent of the window size.

    \section1 Threading

    Same producer/consumer contract as CanLogModel: \l ingest() is called on the
    CAN device thread from CanBusManager::ingestBatch() and only takes the mutex
    long enough to update the accumulators - it never allocates per frame and
    never touches the model API. A 10 Hz GUI timer snapshots those accumulators
    into the display rows and emits one dataChanged for the lot. Bus load
    therefore changes the cost of \l ingest() and nothing else; the UI runs at
    10 Hz whether the bus carries 10 frames a second or 10000.

    \section1 Scope

    Classic-CAN payloads: the first 8 bytes / 64 bits, which is what the
    heatmap shows. CAN FD frames are accepted and counted, but bytes beyond the
    eighth are not visualised.
*/
class CanAnalyzerModel : public QAbstractListModel
{
    Q_OBJECT
    // Registered from main() with qmlRegisterSingletonInstance(), like the rest
    // of the backend. QML_ANONYMOUS exposes the type without claiming the name.
    QML_ANONYMOUS

    Q_PROPERTY(int count READ count NOTIFY countChanged FINAL)
    /*! Self-reference, so QML can write `model: CanAnalyzerModel.rows`. */
    Q_PROPERTY(CanAnalyzerModel *rows READ rows CONSTANT FINAL)

    Q_PROPERTY(int analysisType READ analysisType WRITE setAnalysisType
               NOTIFY analysisTypeChanged FINAL)
    Q_PROPERTY(QStringList analysisTypeNames READ analysisTypeNames CONSTANT FINAL)
    Q_PROPERTY(int byteOrder READ byteOrder WRITE setByteOrder NOTIFY byteOrderChanged FINAL)
    Q_PROPERTY(QStringList byteOrderNames READ byteOrderNames CONSTANT FINAL)

    /*! Number of cells the strip draws: 64 in a bit mode, 8 in a byte mode. */
    Q_PROPERTY(int cellCount READ cellCount NOTIFY analysisTypeChanged FINAL)

    Q_PROPERTY(bool autoSort READ autoSort WRITE setAutoSort NOTIFY autoSortChanged FINAL)
    Q_PROPERTY(int sortColumn READ sortColumn NOTIFY sortChanged FINAL)
    Q_PROPERTY(bool sortDescending READ sortDescending NOTIFY sortChanged FINAL)

    Q_PROPERTY(bool referenceSet READ referenceSet NOTIFY referenceChanged FINAL)
    Q_PROPERTY(bool running READ isRunning WRITE setRunning NOTIFY runningChanged FINAL)

    /*! Row of the first/second selected cell, -1 when nothing is picked. */
    Q_PROPERTY(int selectionRow READ selectionRow NOTIFY selectionChanged FINAL)
    Q_PROPERTY(int firstBit READ firstBit NOTIFY selectionChanged FINAL)
    Q_PROPERTY(int secondBit READ secondBit NOTIFY selectionChanged FINAL)
    Q_PROPERTY(bool canCreateChannel READ canCreateChannel NOTIFY selectionChanged FINAL)
    Q_PROPERTY(QString selectionSummary READ selectionSummary NOTIFY selectionChanged FINAL)

    Q_PROPERTY(QString lastMessage READ lastMessage NOTIFY lastMessageChanged FINAL)

    // -- the analyzer's OWN databases ----------------------------------------
    /*! One entry per loaded file: { name, path, url, messages, signals }.
        Static between load/remove, so a ListView bound to it is not rebuilt at
        the display refresh rate. */
    Q_PROPERTY(QVariantList loadedDbcs READ loadedDbcs NOTIFY databasesChanged FINAL)
    Q_PROPERTY(int databaseCount READ databaseCount NOTIFY databasesChanged FINAL)

    /*!
        Increments once per display refresh.

        This exists so an expanded row's value labels can depend on something
        that changes: a binding onto \l signalText() alone would evaluate once
        and then freeze, because a Q_INVOKABLE is not a bindable property. Naming
        the tick in the binding is the dependency, and it costs one integer
        notification per refresh instead of a dataChanged storm across every
        signal of every open row.
    */
    Q_PROPERTY(int refreshTick READ refreshTick NOTIFY refreshed FINAL)
    Q_PROPERTY(int expandedCount READ expandedCount NOTIFY expandedChanged FINAL)

public:
    enum AnalysisType {
        BitChangeFrequency = 0,  //!< per-bit toggle rate, 64 cells
        ByteValueChange          //!< mean |delta| per byte, 8 cells
    };
    Q_ENUM(AnalysisType)

    enum ByteOrder {
        Intel = 0,               //!< byte 0 leftmost
        Motorola                 //!< byte 7 leftmost
    };
    Q_ENUM(ByteOrder)

    enum SortColumn {
        SortById = 0,
        SortByFrequency,
        SortByChange
    };
    Q_ENUM(SortColumn)

    enum Roles {
        ArbIdRole = Qt::UserRole + 1,
        IdTextRole,
        ExtendedRole,
        DlcRole,
        ChannelRole,
        DirectionRole,
        FrequencyRole,
        ChangeRole,
        NameRole,
        /*! "MessageName (0x1A0)" when the database knows the id, "0x1A0" when
            it does not. One string, so the column never has to decide. */
        ArbTextRole,
        /*! True when the id is in the loaded database. */
        KnownRole,
        ExpandedRole,
        SignalCountRole,
        ColorRole,
        SeenRole
    };

    explicit CanAnalyzerModel(QObject *parent = nullptr);
    ~CanAnalyzerModel() override;

    static CanAnalyzerModel *instance();

    // -- QAbstractListModel ---------------------------------------------------
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    CanAnalyzerModel *rows() { return this; }
    int count() const { return int(m_order.size()); }

    // -- producer -------------------------------------------------------------
    /*!
        \b {CAN device thread.} Folds a batch into the accumulators. Takes the
        mutex once for the whole batch and allocates nothing.
    */
    void ingest(const QVector<CanFrame> &frames);
    void ingest(const CanFrame &frame);

    /*!
        Where \l createChannelBetweenSelectedBits() PUBLISHES a carved-out
        signal - the DBC Editor's document.

        Note what this is not: it is not where names are resolved FROM. The
        analyzer resolves against its own \l loadedDbcs and nothing else, so
        what the Editor happens to have open never changes what this view
        shows. The one direction that remains is outward, because a bit span
        the user just named is worthless if it cannot be saved, and the Editor
        is the only thing in the application that can write a .dbc.
    */
    void setChannelTarget(DbcModel *model);
    void setChannelNames(const QStringList &names);

    /*! Re-resolves every row against the current database list. */
    Q_INVOKABLE void refreshNames();

    // -- database management --------------------------------------------------
    QVariantList loadedDbcs() const;
    int databaseCount() const { return int(m_databases.size()); }

    /*!
        Parses \a url and appends it to this view's private list. Returns false
        and sets \l lastMessage on a parse failure; the existing list is left
        untouched, so a bad file cannot cost the user the databases they had.
    */
    Q_INVOKABLE bool loadAnalyzerDbc(const QUrl &url);
    // NOTE: the parameter is not called "index". QAbstractItemModel::index()
    // is a member function, and a parameter of that name shadows it - the
    // dataChanged() calls in the bodies below stop compiling.
    Q_INVOKABLE void removeAnalyzerDbc(int dbcIndex);
    Q_INVOKABLE void clearAnalyzerDbcs();
    /*! How many arbitration ids currently resolve against database
        \a dbcIndex. The answer to "is this file doing anything?". */
    Q_INVOKABLE int matchedIdCount(int dbcIndex) const;

    // -- display state --------------------------------------------------------
    int analysisType() const { return m_analysisType; }
    void setAnalysisType(int type);
    QStringList analysisTypeNames() const;

    int byteOrder() const { return m_byteOrder; }
    void setByteOrder(int order);
    QStringList byteOrderNames() const;

    int cellCount() const { return m_analysisType == ByteValueChange ? 8 : 64; }

    bool autoSort() const { return m_autoSort; }
    void setAutoSort(bool enabled);
    int sortColumn() const { return m_sortColumn; }
    bool sortDescending() const { return m_sortDescending; }

    bool referenceSet() const { return m_referenceSet; }
    bool isRunning() const { return m_running.load(std::memory_order_relaxed); }
    void setRunning(bool running);

    /*! The selected message's CURRENT view row - recomputed, because sorting
        reindexes rows underneath the selection. */
    int selectionRow() const;
    int firstBit() const { return m_firstBit; }
    int secondBit() const { return m_secondBit; }
    bool canCreateChannel() const { return m_selectionEntry >= 0 && m_firstBit >= 0
                                        && m_secondBit >= 0; }
    QString selectionSummary() const { return m_selectionSummary; }
    QString lastMessage() const { return m_lastMessage; }
    int refreshTick() const { return m_refreshTick; }
    int expandedCount() const { return m_expandedCount; }

    // -- expansion ------------------------------------------------------------
    /*!
        Expansion is stored on the ENTRY, not as a row number in QML.

        Tracking it QML-side looks cheaper and is the same trap the bit
        selection already fell into: sorting reindexes rows, so an expanded
        "row 4" becomes a different message the moment the table re-sorts and
        the open panel shows another message's signals. Keeping it on the entry
        costs one bool and survives every re-sort. It also does not reset the
        model - only the one row's ExpandedRole changes.
    */
    Q_INVOKABLE void toggleExpanded(int row);
    Q_INVOKABLE void setExpanded(int row, bool expanded);
    Q_INVOKABLE void collapseAll();

    /*! Static per-signal facts for an expanded row: name, unit, minimum,
        maximum. Stable while the database is - use it as a Repeater model. */
    Q_INVOKABLE QVariantList signalDescriptors(int row) const;
    /*! Live physical value of the row's i-th signal. */
    Q_INVOKABLE double signalValue(int row, int signalIndex) const;
    /*! The same value formatted for display, e.g. "3500.0 rpm". */
    Q_INVOKABLE QString signalText(int row, int signalIndex) const;

    // -- commands -------------------------------------------------------------
    /*!
        Freezes the current activity as the zero point. Afterwards a row shows
        only what is NEW: park the car, press this, turn the wheel, and the
        wheel-angle bits are the only ones lit.
    */
    Q_INVOKABLE void setReference();
    Q_INVOKABLE void clearReference();
    /*! Drops every row and every accumulator. */
    Q_INVOKABLE void reset();

    /*! One-shot sort ("Sort by this column"). */
    Q_INVOKABLE void sortBy(int column, bool descending = true);
    Q_INVOKABLE void unsort();

    Q_INVOKABLE void selectFirstBit(int row, int cell);
    Q_INVOKABLE void selectSecondBit(int row, int cell);
    Q_INVOKABLE void clearSelection();

    /*!
        Turns the selected cell span into a real DBC signal on the message for
        that id, creating the message when the database does not have it yet.
        This is the point of the whole view: a lit-up run of bits becomes a
        named, plottable channel. Returns the signal name, or an empty string.
    */
    Q_INVOKABLE QString createChannelBetweenSelectedBits(const QString &name = {});

    /*! Row lookups for the QML side of the table. */
    Q_INVOKABLE QVariantMap get(int row) const;

    // -- painting -------------------------------------------------------------
    // C++-only, called by BitStripItem. Returning pointers into the display row
    // rather than a QVariantList is what keeps the heatmap allocation-free:
    // 25 visible strips x 64 cells x 10 Hz would otherwise be 16000 QVariant
    // constructions a second for data that is already contiguous floats.
    /*! Normalised 0..1 activity per cell, in display (byte-order) order. */
    const float *cellActivity(int row) const;
    /*! Cell values: 0/1 in a bit mode, 0..255 in a byte mode. */
    const quint8 *cellValues(int row) const;
    bool rowIsValid(int row) const { return row >= 0 && row < int(m_order.size()); }

signals:
    void countChanged();
    void analysisTypeChanged();
    void byteOrderChanged();
    void autoSortChanged();
    void sortChanged();
    void referenceChanged();
    void runningChanged();
    void selectionChanged();
    void lastMessageChanged();
    void expandedChanged();
    /*! The set of loaded databases changed (load, remove, clear). */
    void databasesChanged();
    /*! The database behind the names/signal lists changed. Distinct from
        refreshed(), which fires 10x a second - a panel that rebuilt its
        delegate list on refreshed() would rebuild them six hundred times a
        minute for data that changes when a .dbc is loaded. */
    void namesChanged();
    /*! One pulse per display refresh; BitStripItem repaints on it. */
    void refreshed();

private:
    /*! 2 s. Long enough that a 10 Hz message contributes ~20 samples (which is
        what makes a Change score of 12/17 or 17/23 readable), short enough that
        letting go of the wheel visibly cools the row. */
    static constexpr qint64 WindowNs = 2'000'000'000LL;
    /*! Hard cap per id, so a flooding node cannot grow the window without
        bound. Past this the window is "the last N frames" instead of "2 s". */
    static constexpr int MaxSamples = 512;
    static constexpr int AnalysedBytes = 8;
    static constexpr int AnalysedBits = AnalysedBytes * 8;
    static constexpr int RefreshMs = 100;

    /*! One received frame, reduced to what the counters need. */
    struct Sample {
        qint64 timestampNs = 0;
        quint64 toggleMask = 0;                    //!< bits that differed from the previous frame
        std::array<quint8, AnalysedBytes> delta {}; //!< |byte - previous byte|
    };

    /*! Everything tracked for one arbitration id. Accumulators are written on
        the device thread under \c m_mutex; the display half is written on the
        GUI thread only. */
    struct Entry {
        // -- identity
        quint32 id = 0;
        bool extended = false;
        quint8 dlc = 0;
        quint8 channel = 0;
        CanFrame::Direction direction = CanFrame::Rx;

        // -- producer side
        std::deque<Sample> window;
        std::array<quint32, AnalysedBits> toggleCount {};
        std::array<quint32, AnalysedBytes> deltaSum {};
        quint32 changedCount = 0;
        std::array<quint8, AnalysedBytes> lastPayload {};
        bool hasPrevious = false;
        qint64 lastSeenNs = 0;
        quint64 totalFrames = 0;

        // -- display side (GUI thread)
        double frequency = 0.0;
        double rawChange = 0.0;
        double change = 0.0;
        double referenceChange = 0.0;
        std::array<float, AnalysedBits> rawBitActivity {};
        std::array<float, AnalysedBytes> rawByteActivity {};
        std::array<float, AnalysedBits> refBitActivity {};
        std::array<float, AnalysedBytes> refByteActivity {};
        std::array<quint8, AnalysedBytes> displayPayload {};
        // Laid out for the painter in the current byte order, rebuilt on refresh.
        std::array<float, AnalysedBits> cellActivity {};
        std::array<quint8, AnalysedBits> cellValues {};
        QString name;              //!< name carried on the frame, if any

        // -- database cross-reference (GUI thread)
        QString dbcName;           //!< message name from the matching database
        /*! The database that claimed this id. Held per entry rather than
            looked up per decode: the list is stable between edits, and pairing
            signalKeys with the decoder that issued them is what stops a
            removed file leaving keys behind that index into another. */
        const CanDecoder *decoder = nullptr;
        int databaseIndex = -1;
        bool known = false;        //!< some loaded database has this id
        bool nameResolved = false;
        bool expanded = false;
        QVector<int> signalKeys;   //!< decoder keys, in the database's order
        QVector<double> signalValues;
    };

    void refresh();
    void recomputeEntry(Entry &entry, qint64 nowNs);
    /*!
        Decodes the signals of the rows that are actually open.

        \b {There is no decoded-value cache to reuse.} CanBusManager only
        decodes at all while the Graph Plot is tracking something
        (\c m_decodingEnabled follows GraphDataModel::isActive), and a
        DecodedSample is {key, timestamp, value} that is consumed and dropped -
        nothing keeps the latest value of every signal. Building such a cache
        would mean decoding every signal of every frame at full bus rate, always
        on, which is exactly the second decoding loop we want to avoid.

        So this decodes on demand instead: only expanded rows, only from the
        last payload the analyzer already stores, and only at the 10 Hz display
        refresh. Two open rows with ten signals each cost two hundred
        extractions a second, against fifty thousand for a permanent cache on a
        5000 fps bus. It goes through CanDecoder::decode(), so the bit maths is
        the existing one - reused, not reimplemented.
    */
    void decodeExpandedRows();
    /*!
        Finds the first loaded database that knows this id and pins the entry to
        it. FIRST MATCH WINS, in load order - with two overlapping databases the
        earlier one owns the id, which is why the panel lists them in order and
        removing/re-adding is how you change the priority.
    */
    void resolveName(Entry &entry);
    /*! Every entry's resolution and signal list is stale; drop it. */
    void invalidateResolution();
    void buildCells(Entry &entry) const;
    void rebuildOrder();
    void applySort();
    Entry *entryForRow(int row);
    const Entry *entryForRow(int row) const;
    void setLastMessage(const QString &message);
    void updateSelectionSummary();
    /*! Display cell -> underlying bit index (0..63, bit 0 = byte 0 LSB). */
    int bitForCell(int cell) const;

    static CanAnalyzerModel *s_instance;

    mutable QMutex m_mutex;                  //!< guards the producer half of every Entry
    QHash<quint32, int> m_indexByKey;        //!< frame key -> index into m_entries
    // std::deque, NOT std::vector: the device thread appends entries while the
    // GUI thread holds references to existing ones for painting. A vector would
    // reallocate and leave the painter reading freed memory; a deque never
    // invalidates a reference on push_back.
    std::deque<Entry> m_entries;
    std::vector<int> m_order;                //!< display row -> m_entries index

    /*! One loaded file. The CanDecoder is heap-held because it owns a
        QReadWriteLock and so cannot be copied, and because entries store a raw
        pointer to it that must survive the vector growing. */
    struct AnalyzerDatabase {
        QString name;
        QString path;
        QUrl url;
        int messageCount = 0;
        int signalCount = 0;
        std::unique_ptr<CanDecoder> decoder;
    };

    QTimer *m_refreshTimer = nullptr;
    /*! Export target only - see setChannelTarget(). */
    DbcModel *m_channelTarget = nullptr;
    std::vector<AnalyzerDatabase> m_databases;
    QStringList m_channelNames;

    int m_analysisType = BitChangeFrequency;
    int m_byteOrder = Intel;
    bool m_autoSort = false;
    int m_sortColumn = SortById;
    bool m_sortDescending = true;
    bool m_referenceSet = false;
    std::atomic_bool m_running { true };

    /*! Index into m_entries, NOT a view row: a row number stops meaning the
        same message the moment the table re-sorts, and the selection has to
        survive that. */
    int m_selectionEntry = -1;
    int m_firstBit = -1;
    int m_secondBit = -1;
    QString m_selectionSummary;
    QString m_lastMessage;
    int m_refreshTick = 0;
    int m_expandedCount = 0;
};
