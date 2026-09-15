#pragma once

#include <QAbstractListModel>
#include <QElapsedTimer>
#include <QHash>
#include <QMutex>
#include <QVector>
#include <QtQml/qqmlregistration.h>

#include "core/CanFrame.h"
#include "dbc/CanDecoder.h"

class DbcModel;
class QTimer;

/*!
    \brief The database flattened into one row list: messages, and the signals
           of the messages that are expanded.

    \section2 Why a separate model and not roles on DbcModel

    DbcModel is a list of messages whose signals hang off a per-message child
    model, which is the right shape for a form and the wrong shape for a grid:
    a single table cannot walk two levels. Rather than teach every view to
    nest, this model publishes the ONE view a hierarchical grid actually needs
    - a flat sequence of rows, each tagged with its \l kind and \l depth - and
    leaves DbcModel as the single source of truth. Nothing is copied except the
    two indices needed to reach back into it, so an edit made through
    DbcModel is visible here immediately and there is no second copy to
    desynchronise.

    \section2 Rebuild policy: compare, then choose

    Every change in the database funnels into one coalesced \c rebuild().
    It recomputes the row vector and compares it to the current one:

    \list
    \li same structure -> \c dataChanged over the range. A model RESET would
        also be correct, and would also throw away the selection, the scroll
        position and any open editor - so typing one character in a signal name
        would scroll the user back to the top.
    \li different structure -> a real reset.
    \endlist

    \section2 Live values

    \l liveValuesEnabled gates the whole live path, and QML turns it on only
    while the editor page is actually mounted. That matters because the
    alternative is decoding every frame on the bus to populate a column nobody
    is looking at.

    When it is on, \l ingest() runs on the DEVICE thread (CanBusManager fans
    out with a direct connection), so it decodes into a scratch buffer and
    parks the result behind a mutex. The GUI thread drains that at 5 Hz - fast
    enough to read as live, slow enough that a 1 kHz bus does not turn into
    1000 model updates a second. This is the same throttle CanSenderModel's
    sentCount and the graph's readouts use.
*/
class DbcFlatModel : public QAbstractListModel
{
    Q_OBJECT
    // Published from main() with qmlRegisterSingletonInstance(); see the note
    // in DbcModel.h for why this is not QML_SINGLETON.
    QML_ANONYMOUS

    Q_PROPERTY(int count READ count NOTIFY countChanged FINAL)
    /*! Turn on only while a view showing the value column is on screen. */
    Q_PROPERTY(bool liveValuesEnabled READ liveValuesEnabled
               WRITE setLiveValuesEnabled NOTIFY liveValuesEnabledChanged FINAL)

public:
    enum Kind {
        MessageRow = 0,
        SignalRow
    };
    Q_ENUM(Kind)

    enum Role {
        KindRole = Qt::UserRole + 1,
        DepthRole,
        MessageIndexRole,
        SignalIndexRole,
        ArbTextRole,        //!< "A7h" / "1FFFFFFFh"
        FormatTextRole,     //!< "Standard" / "Extended"
        BitsTextRole,       //!< "DLC = 8" on a message, "12 (10)" on a signal
        FrequencyTextRole,  //!< "0.0 Hz", measured, not the DBC cycle time
        NameRole,
        DescriptionRole,
        UnitRole,
        MinimumRole,
        MaximumRole,
        LiveValueRole,      //!< qQNaN() until a frame has actually been seen
        LiveTextRole,       //!< the VAL_ description when mapped, else 3 dp
        LiveFractionRole,   //!< 0..1 position between min and max, for the bar
        ColorIndexRole,     //!< index into Theme.seriesPalette - stable per row
        ExpandedRole,
        HasChildrenRole,
        SignalCountRole
    };
    Q_ENUM(Role)

    explicit DbcFlatModel(QObject *parent = nullptr);
    ~DbcFlatModel() override;

    static DbcFlatModel *instance();

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const { return m_rows.size(); }

    /*! Wired in main(). The model watches every structural signal it emits. */
    void setSource(DbcModel *source);
    /*! Wired in main(); borrowed from CanBusManager, never owned. */
    void setDecoder(const CanDecoder *decoder);

    bool liveValuesEnabled() const { return m_liveEnabled; }
    void setLiveValuesEnabled(bool enabled);

    /*! Called on the device thread by CanBusManager. Returns immediately when
        live values are off, which is the whole point of the gate. */
    void ingest(const QVector<CanFrame> &frames);

    // -- row lookup, for a view that wants to scroll to something -------------
    Q_INVOKABLE int rowForMessage(int messageIndex) const;
    Q_INVOKABLE int rowForSignal(int messageIndex, int signalIndex) const;

    /*! Newest decoded value for one signal, or an invalid QVariant (undefined
        in QML) when live values are off or nothing has arrived yet. Lets the
        setup dialog show a reading without holding a row index that a rebuild
        could invalidate under it. */
    Q_INVOKABLE QVariant liveValueAt(int messageIndex, int signalIndex) const;

public slots:
    /*! Expand/collapse from the grid's disclosure triangle. */
    void toggleExpanded(int row);

    /*!
        Re-resolve every cached \c decoderKey. Wired to
        \c CanBusManager::decoderRebuilt() in main().

        \b {Why this exists.} DbcModel::applyDatabase() emits \c modelReset
        (from endResetModel()) BEFORE it emits \c loaded / \c databaseChanged.
        This model listens to the first group, CanBusManager to the second, and
        both coalesce onto a single-shot zero timer - which Qt fires in the
        order the timers were STARTED. So on every single load the sequence is:

          1. rebuild() here, resolving keys against the OUTGOING decode table;
          2. CanBusManager::refreshDecoder(), which replaces that table.

        The keys cached in step 1 are stale the instant step 2 runs, and
        nothing used to re-resolve them - so after loading a database into an
        empty editor, every live value and colour was missing. It looked like a
        race in whatever had triggered the load (importing a session file was where it
        showed up), but the load path is irrelevant: the same two timers start
        in the same order however the database arrives. Loading a SECOND time
        appeared to fix it only because by then the decoder already held the
        right table, so step 1 happened to resolve correctly.

        Cheap by design - keys only, no structural rebuild, so expansion state
        and scroll position survive. */
    void decoderRebuilt();

signals:
    void countChanged();
    void liveValuesEnabledChanged();

private:
    struct Row
    {
        int kind = MessageRow;
        int messageIndex = -1;
        int signalIndex = -1;      //!< -1 on a message row
        int colorIndex = 0;
        int decoderKey = -1;       //!< CanDecoder key, -1 while unresolved

        /*! Identity for the structure comparison - deliberately NOT including
            decoderKey, which moves whenever the decoder rebuilds and would
            otherwise force a reset on every DBC edit. */
        bool sameAs(const Row &other) const
        {
            return kind == other.kind
                && messageIndex == other.messageIndex
                && signalIndex == other.signalIndex;
        }
    };

    QVector<Row> buildRows() const;
    void scheduleRebuild();
    void rebuild();
    void flushLive();
    void resolveDecoderKeys();

    static DbcFlatModel *s_instance;

    DbcModel *m_source = nullptr;
    const CanDecoder *m_decoder = nullptr;

    QVector<Row> m_rows;
    QTimer *m_rebuildTimer = nullptr;

    // -- live values ---------------------------------------------------------
    bool m_liveEnabled = false;
    QTimer *m_liveTimer = nullptr;
    QElapsedTimer m_liveWindow;

    /*! Written on the device thread, drained on the GUI thread. */
    QMutex m_liveMutex;
    QHash<int, double> m_pendingValues;     //!< decoder key -> newest value
    QHash<quint32, int> m_pendingCounts;    //!< frame key -> frames this window
    /*! Reused across batches so a busy bus does not allocate per call. */
    QVector<DecodedSample> m_decodeScratch;

    /*! GUI-side copies. */
    QHash<int, double> m_liveValues;
    QHash<quint32, double> m_liveRates;     //!< frame key -> smoothed Hz
};
