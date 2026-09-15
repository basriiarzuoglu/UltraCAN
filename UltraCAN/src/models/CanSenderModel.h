#pragma once

#include <QAbstractListModel>
#include <QSet>
#include <QStringList>
#include <QVariantMap>
#include <QVector>
#include <QtQml/qqmlregistration.h>

#include <array>

#include "core/CanFrame.h"

class CanBusManager;
class DbcModel;
class QTimer;

/*!
    \brief The transmit-job list: what to send, how often, and the counters.

    Replaces the QML CanSenderDummyModel. Each job owns a QTimer, so every job
    keeps its own period and phase instead of aliasing to one global tick - two
    jobs at 10 ms and 15 ms stay 10 ms and 15 ms apart rather than snapping to a
    common grid.

    \note The timers live on the GUI thread and the write is handed to the
          device thread asynchronously, so cycle times are as accurate as the
          Qt event loop (sub-millisecond when idle, tens of milliseconds under a
          heavy repaint). That is fine for stimulus and diagnostics; a hard
          real-time gateway would move the timers onto the device thread, which
          is why the write path already goes through
          CanBusManager::writeFrame() rather than touching the device directly.

    Roles are the ones CanMessageSenderView.qml already reads, so the view
    changes only where it used to do the transmitting itself.
*/
class CanSenderModel : public QAbstractListModel
{
    Q_OBJECT
    // Registered by instance from main() - see the note in main.cpp.
    QML_ANONYMOUS

    /*! Returns the model itself, so `jobs.count` / `jobs.get(i)` keep working. */
    Q_PROPERTY(CanSenderModel *jobs READ jobs CONSTANT FINAL)
    Q_PROPERTY(int count READ count NOTIFY countChanged FINAL)
    Q_PROPERTY(qint64 totalSent READ totalSent NOTIFY statisticsChanged FINAL)
    Q_PROPERTY(int armedCount READ enabledCount NOTIFY statisticsChanged FINAL)
    Q_PROPERTY(QStringList channelNames READ channelNames CONSTANT FINAL)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged FINAL)
    /*! False while the bus is down: nothing is written and no counter moves. */
    Q_PROPERTY(bool canTransmit READ canTransmit NOTIFY canTransmitChanged FINAL)

public:
    enum Role {
        JobIdRole = Qt::UserRole + 1,
        NameRole, CanIdRole, IdTextRole, ExtendedRole, ChannelRole,
        DlcRole, PayloadRole, CycleMsRole, EnabledRole, SentCountRole, OriginRole
    };
    Q_ENUM(Role)

    explicit CanSenderModel(QObject *parent = nullptr);
    ~CanSenderModel() override;

    static CanSenderModel *instance();

    /*! Wired in main(). Without a bus manager the model still holds jobs but
        cannot transmit, and says so through \l lastError. */
    void setBusManager(CanBusManager *manager);
    /*! Wired in main(). Needed only by \l encodePayload(). */
    void setDbcModel(DbcModel *model);

    // -- QAbstractListModel ----------------------------------------------------
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    CanSenderModel *jobs() { return this; }
    int count() const { return m_jobs.size(); }
    qint64 totalSent() const { return m_totalSent; }
    QStringList channelNames() const;
    QString lastError() const { return m_lastError; }
    /*! A job can only go out when a device is actually connected. */
    bool canTransmit() const;

    // -- job management (called from QML) --------------------------------------
    /*!
        Adds a job from the builder panes. Recognised keys: name, canId,
        extended, channel, dlc, payload ("AA BB .."), cycleMs, origin.
        Returns the new row, or -1.
    */
    /*!
        Adds a transmit job, or UPDATES the one already holding that frame.

        A transmit list is not a log: two rows sending the same frame on the
        same channel are not two facts, they are one job entered twice, and on
        the wire they are two nodes fighting for the same arbitration id.
        Re-adding therefore replaces payload, DLC, cycle time and name in
        place, keeps the row's identity and its sent counter, and re-arms the
        timer at the new interval.

        The match key is (id, extended, channel), NOT (id, channel). A
        standard 0x100 and an extended 0x100 are different arbitration fields
        and can legitimately coexist on one bus. Matching on the id alone
        would silently merge them and make one of the two unsendable. */
    Q_INVOKABLE int addJob(const QVariantMap &job);
    /*! Row already transmitting this exact frame, or -1. */
    Q_INVOKABLE int indexOfFrame(quint32 canId, bool extended, int channel) const;
    Q_INVOKABLE void removeJob(int index);
    Q_INVOKABLE void clearJobs();
    Q_INVOKABLE QVariantMap jobAt(int index) const;
    Q_INVOKABLE QVariantMap get(int index) const { return jobAt(index); }
    Q_INVOKABLE void setEnabled(int index, bool enabled);
    Q_INVOKABLE void setPayload(int index, const QString &hex);
    Q_INVOKABLE int enabledCount() const;

    // -- transmission ----------------------------------------------------------
    /*! Sends job \a index once, now. Returns false if it could not be written. */
    Q_INVOKABLE bool sendOnce(int index);
    /*! Sends every job once. Returns how many went out. */
    Q_INVOKABLE int sendAllOnce();

    // -- helpers the view already calls ----------------------------------------
    Q_INVOKABLE QString formatId(quint32 id, bool extended = false) const;
    Q_INVOKABLE QString formatCycle(int cycleMs) const;
    Q_INVOKABLE QString bytesToHex(const QVariantList &bytes) const;
    /*!
        Encodes DBC signal values into a payload, using the same bit layout the
        decoder reads: \a values maps signal index (as a string key) to a
        physical value. Returns "AA BB .." ready for \l addJob().
    */
    Q_INVOKABLE QString encodePayload(int messageIndex, const QVariantMap &values) const;

signals:
    void countChanged();
    void canTransmitChanged();
    void statisticsChanged();
    void lastErrorChanged();
    /*! A job went out (or failed to). Useful for logging and tests. */
    void jobTransmitted(int index, bool ok);

private:
    struct TxJob
    {
        int jobId = 0;
        QString name;
        quint32 canId = 0;
        bool extended = false;
        quint8 channel = 0;
        quint8 dlc = 8;
        std::array<quint8, 64> payload {};
        int cycleMs = 0;
        bool enabled = false;
        qint64 sentCount = 0;
        QString origin;
        QTimer *timer = nullptr;      //!< owned by this model, one per cyclic job
    };

    int indexOfJobId(int jobId) const;
    /*! Folds \a fresh into the job at \a row, preserving identity. */
    int updateJob(int row, const TxJob &fresh);
    /*! Starts or stops every armed job's timer to match the bus state. */
    void handleConnectionChanged();
    void startTimer(int index);
    void stopTimer(int index);
    bool transmit(int index);
    void setLastError(const QString &message);
    static int parseHexPayload(const QString &text, std::array<quint8, 64> &out);
    static QString payloadText(const std::array<quint8, 64> &payload, int dlc);

    static CanSenderModel *s_instance;

    CanBusManager *m_busManager = nullptr;
    DbcModel *m_dbcModel = nullptr;

    QVector<TxJob> m_jobs;
    int m_nextJobId = 1;
    qint64 m_totalSent = 0;
    QString m_lastError;

    // Counter updates are throttled: a 10 ms job would otherwise emit
    // dataChanged 100 times a second per row for a number nobody can read that
    // fast. The true count is always exact; only its repaint is coalesced.
    QTimer *m_counterFlushTimer = nullptr;
    QSet<int> m_dirtyRows;
};
