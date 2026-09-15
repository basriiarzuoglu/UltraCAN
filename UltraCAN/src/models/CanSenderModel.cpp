#include "CanSenderModel.h"

#include <QTimer>

#include <utility>

#include "core/CanBusManager.h"
#include "dbc/CanDecoder.h"
#include "models/DbcModel.h"

CanSenderModel *CanSenderModel::s_instance = nullptr;

CanSenderModel::CanSenderModel(QObject *parent)
    : QAbstractListModel(parent)
{
    if (!s_instance) {
        s_instance = this;
    } else {
        qCritical("CanSenderModel: a SECOND instance was created (%p; first was %p).",
                  static_cast<void *>(this), static_cast<void *>(s_instance));
    }

    m_counterFlushTimer = new QTimer(this);
    m_counterFlushTimer->setInterval(100);
    connect(m_counterFlushTimer, &QTimer::timeout, this, [this] {
        if (m_dirtyRows.isEmpty()) {
            m_counterFlushTimer->stop();
            return;
        }
        for (int row : std::as_const(m_dirtyRows)) {
            if (row >= 0 && row < m_jobs.size())
                emit dataChanged(index(row), index(row), { SentCountRole });
        }
        m_dirtyRows.clear();
        emit statisticsChanged();
    });
}

CanSenderModel::~CanSenderModel()
{
    if (s_instance == this)
        s_instance = nullptr;
}

CanSenderModel *CanSenderModel::instance()
{
    return s_instance;
}

void CanSenderModel::setBusManager(CanBusManager *manager)
{
    if (m_busManager == manager)
        return;

    if (m_busManager)
        disconnect(m_busManager, nullptr, this, nullptr);

    m_busManager = manager;

    if (m_busManager) {
        // Arming a job is a user intent that outlives a cable being unplugged:
        // keep `enabled` as it is, but stop the timers while the bus is down
        // and start them again on reconnect.
        connect(m_busManager, &CanBusManager::connectionChanged,
                this, &CanSenderModel::handleConnectionChanged);
    }
    handleConnectionChanged();
}

bool CanSenderModel::canTransmit() const
{
    return m_busManager && m_busManager->isConnected();
}

void CanSenderModel::handleConnectionChanged()
{
    const bool online = canTransmit();
    for (int i = 0; i < m_jobs.size(); ++i) {
        if (!m_jobs.at(i).enabled)
            continue;
        if (online)
            startTimer(i);
        else
            stopTimer(i);
    }

    if (!online)
        setLastError(QString());     // the bus state already says why

    emit canTransmitChanged();
    emit statisticsChanged();
}

void CanSenderModel::setDbcModel(DbcModel *model)
{
    m_dbcModel = model;
}

QStringList CanSenderModel::channelNames() const
{
    return m_busManager ? m_busManager->channelNames()
                        : QStringList { QStringLiteral("CAN1"), QStringLiteral("CAN2") };
}

// ------------------------------------------------------------------- model

int CanSenderModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_jobs.size();
}

QHash<int, QByteArray> CanSenderModel::roleNames() const
{
    // Exactly the role names CanMessageSenderView.qml already reads.
    return {
        { JobIdRole,     QByteArrayLiteral("jobId")     },
        { NameRole,      QByteArrayLiteral("name")      },
        { CanIdRole,     QByteArrayLiteral("canId")     },
        { IdTextRole,    QByteArrayLiteral("idText")    },
        { ExtendedRole,  QByteArrayLiteral("extended")  },
        { ChannelRole,   QByteArrayLiteral("channel")   },
        { DlcRole,       QByteArrayLiteral("dlc")       },
        { PayloadRole,   QByteArrayLiteral("payload")   },
        { CycleMsRole,   QByteArrayLiteral("cycleMs")   },
        { EnabledRole,   QByteArrayLiteral("enabled")   },
        { SentCountRole, QByteArrayLiteral("sentCount") },
        { OriginRole,    QByteArrayLiteral("origin")    }
    };
}

QVariant CanSenderModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_jobs.size())
        return {};

    const TxJob &job = m_jobs.at(index.row());
    switch (role) {
    case JobIdRole:     return job.jobId;
    case NameRole:      return job.name;
    case CanIdRole:     return job.canId;
    case IdTextRole:    return formatId(job.canId, job.extended);
    case ExtendedRole:  return job.extended;
    case ChannelRole:   return channelNames().value(job.channel, QStringLiteral("CAN?"));
    case DlcRole:       return int(job.dlc);
    case PayloadRole:   return payloadText(job.payload, job.dlc);
    case CycleMsRole:   return job.cycleMs;
    case EnabledRole:   return job.enabled;
    case SentCountRole: return job.sentCount;
    case OriginRole:    return job.origin;
    default:            return {};
    }
}

// ------------------------------------------------------------ job handling

int CanSenderModel::addJob(const QVariantMap &job)
{
    TxJob entry;
    entry.jobId = m_nextJobId++;
    entry.name = job.value(QStringLiteral("name")).toString();
    entry.canId = job.value(QStringLiteral("canId")).toUInt();
    entry.extended = job.value(QStringLiteral("extended")).toBool();

    // An identifier above 0x7FF cannot be carried by an 11-bit frame, so this
    // is arithmetic, not a preference. Without the flag QCanBusFrame is invalid
    // and the driver drops it silently - which looks exactly like a working
    // sender that the bus ignores.
    if (entry.canId > 0x7FFu && !entry.extended) {
        entry.extended = true;
        qWarning("CanSenderModel: id 0x%X needs the extended format; enabling it.",
                 entry.canId);
    }
    entry.canId &= entry.extended ? 0x1FFFFFFFu : 0x7FFu;
    entry.origin = job.value(QStringLiteral("origin")).toString();
    entry.cycleMs = job.value(QStringLiteral("cycleMs"), 0).toInt();

    const QString channel = job.value(QStringLiteral("channel")).toString();
    const int channelIndex = channelNames().indexOf(channel);
    entry.channel = quint8(channelIndex >= 0 ? channelIndex : 0);

    const int parsed = parseHexPayload(job.value(QStringLiteral("payload")).toString(),
                                       entry.payload);
    entry.dlc = quint8(qBound(0, job.value(QStringLiteral("dlc"), parsed).toInt(), 64));

    // Cyclic jobs arm themselves; single-shot jobs wait for the Send button.
    entry.enabled = entry.cycleMs > 0;

    // -- one row per frame ------------------------------------------------
    //
    // Looked up AFTER the normalisation above, never before: the id has just
    // been masked and may have had `extended` forced on (anything over 0x7FF
    // cannot ride an 11-bit frame). Matching on the raw submitted values would
    // miss a re-add of the very same frame whenever the two spellings differ.
    const int existing = indexOfFrame(entry.canId, entry.extended, entry.channel);
    if (existing >= 0)
        return updateJob(existing, entry);

    beginInsertRows(QModelIndex(), m_jobs.size(), m_jobs.size());
    m_jobs.append(entry);
    endInsertRows();

    if (m_jobs.last().enabled)
        startTimer(m_jobs.size() - 1);

    emit countChanged();
    emit statisticsChanged();
    return m_jobs.size() - 1;
}

int CanSenderModel::indexOfFrame(quint32 canId, bool extended, int channel) const
{
    for (int i = 0; i < m_jobs.size(); ++i) {
        const TxJob &job = m_jobs.at(i);
        if (job.canId == canId && job.extended == extended && int(job.channel) == channel)
            return i;
    }
    return -1;
}

int CanSenderModel::updateJob(int row, const TxJob &fresh)
{
    if (row < 0 || row >= m_jobs.size())
        return -1;

    TxJob &job = m_jobs[row];

    // What is REPLACED: everything the user just typed.
    job.payload = fresh.payload;
    job.dlc = fresh.dlc;
    job.cycleMs = fresh.cycleMs;
    job.origin = fresh.origin;
    job.enabled = fresh.enabled;
    // A blank name must not wipe one that is already there. Raw mode can
    // submit an empty string; the row may be carrying a message name the
    // database supplied on an earlier add.
    if (!fresh.name.isEmpty())
        job.name = fresh.name;

    // What is KEPT: the row's identity.
    //
    //   jobId     - the timer's lambda looks the row up by it, and QML may be
    //               holding it as a selection.
    //   sentCount - this is the same job as before. Resetting the counter
    //               would say the frame had never gone out, which is the one
    //               thing on this page that must not lie.
    //   timer     - reused rather than recreated, so no connect() leaks and no
    //               gap in transmission across the update.

    // The payload needs no special handling: transmit() reads it out of the
    // job on every tick, so the next frame on the wire already carries the new
    // bytes. The INTERVAL does need this - QTimer keeps the period it was
    // started with until it is started again.
    if (job.enabled && job.cycleMs > 0)
        startTimer(row);        // start() on a running timer re-arms at the new period
    else
        stopTimer(row);

    // Empty roles vector = "every role" - name, payload, DLC, cycle and armed
    // state can all have moved, and listing them is a maintenance trap the
    // moment a role is added.
    emit dataChanged(index(row), index(row));
    emit statisticsChanged();
    return row;
}

void CanSenderModel::removeJob(int index)
{
    if (index < 0 || index >= m_jobs.size())
        return;

    stopTimer(index);

    beginRemoveRows(QModelIndex(), index, index);
    m_jobs.removeAt(index);
    endRemoveRows();

    m_dirtyRows.clear();          // rows shifted; the indexes are meaningless now
    emit countChanged();
    emit statisticsChanged();
}

void CanSenderModel::clearJobs()
{
    if (m_jobs.isEmpty())
        return;

    for (int i = 0; i < m_jobs.size(); ++i)
        stopTimer(i);

    beginResetModel();
    m_jobs.clear();
    endResetModel();

    m_dirtyRows.clear();
    emit countChanged();
    emit statisticsChanged();
}

QVariantMap CanSenderModel::jobAt(int index) const
{
    QVariantMap map;
    if (index < 0 || index >= m_jobs.size())
        return map;

    const QModelIndex idx = this->index(index);
    const auto roles = roleNames();
    for (auto it = roles.cbegin(); it != roles.cend(); ++it)
        map.insert(QString::fromUtf8(it.value()), data(idx, it.key()));
    return map;
}

void CanSenderModel::setEnabled(int index, bool enabled)
{
    if (index < 0 || index >= m_jobs.size())
        return;

    TxJob &job = m_jobs[index];
    if (job.cycleMs <= 0)
        enabled = false;             // a manual job can never be armed
    if (job.enabled == enabled)
        return;

    job.enabled = enabled;
    if (enabled)
        startTimer(index);
    else
        stopTimer(index);

    emit dataChanged(this->index(index), this->index(index), { EnabledRole });
    emit statisticsChanged();
}

void CanSenderModel::setPayload(int index, const QString &hex)
{
    if (index < 0 || index >= m_jobs.size())
        return;

    TxJob &job = m_jobs[index];
    job.payload.fill(0);
    parseHexPayload(hex, job.payload);
    emit dataChanged(this->index(index), this->index(index), { PayloadRole });
}

int CanSenderModel::enabledCount() const
{
    int n = 0;
    for (const TxJob &job : m_jobs)
        if (job.enabled && job.cycleMs > 0)
            ++n;
    return n;
}

int CanSenderModel::indexOfJobId(int jobId) const
{
    for (int i = 0; i < m_jobs.size(); ++i)
        if (m_jobs.at(i).jobId == jobId)
            return i;
    return -1;
}

// --------------------------------------------------------------- the timers

void CanSenderModel::startTimer(int index)
{
    if (index < 0 || index >= m_jobs.size())
        return;

    TxJob &job = m_jobs[index];
    if (job.cycleMs <= 0)
        return;
    if (!canTransmit())
        return;             // armed, but the bus is down - see handleConnectionChanged()

    if (!job.timer) {
        job.timer = new QTimer(this);
        job.timer->setTimerType(Qt::PreciseTimer);
        const int jobId = job.jobId;
        // Look the job up by id, not by the index captured here: rows shift
        // whenever a job above this one is removed.
        connect(job.timer, &QTimer::timeout, this, [this, jobId] {
            const int row = indexOfJobId(jobId);
            if (row >= 0)
                transmit(row);
        });
    }
    job.timer->start(job.cycleMs);
}

void CanSenderModel::stopTimer(int index)
{
    if (index < 0 || index >= m_jobs.size())
        return;
    if (QTimer *timer = m_jobs[index].timer)
        timer->stop();
}

// ------------------------------------------------------------- transmission

bool CanSenderModel::transmit(int index)
{
    if (index < 0 || index >= m_jobs.size())
        return false;

    TxJob &job = m_jobs[index];

    CanFrame frame;
    frame.timestampNs = 0;               // stamped by the manager's bus clock
    frame.direction = CanFrame::Tx;
    frame.id = job.canId;
    frame.extended = job.extended;
    frame.channel = job.channel;
    frame.dlc = job.dlc;
    frame.payload = job.payload;
    frame.name = job.name;

    if (!m_busManager) {
        setLastError(tr("No bus manager - the backend is not wired up."));
        emit jobTransmitted(index, false);
        return false;
    }

    // STRICT GUARD. Nothing is written, nothing is counted and nothing is
    // echoed into the trace while the bus is down. A counter that climbs on a
    // disconnected bus is worse than useless - it reads as proof the frames
    // went out.
    if (!m_busManager->isConnected()) {
        setLastError(tr("Bus is disconnected - %1 was not sent.")
                     .arg(formatId(job.canId, job.extended)));
        emit jobTransmitted(index, false);
        return false;
    }

    // sendFrame() writes to the hardware and echoes the frame into the trace,
    // so the Logger shows what went out even on a bus that does not loop
    // transmissions back.
    const bool ok = m_busManager->sendFrame(frame);
    if (!ok) {
        setLastError(tr("Write failed for %1.").arg(formatId(job.canId, job.extended)));
        emit jobTransmitted(index, false);
        return false;
    }

    ++job.sentCount;
    ++m_totalSent;
    m_dirtyRows.insert(index);
    if (!m_counterFlushTimer->isActive())
        m_counterFlushTimer->start();

    emit jobTransmitted(index, true);
    return true;
}

bool CanSenderModel::sendOnce(int index)
{
    return transmit(index);
}

int CanSenderModel::sendAllOnce()
{
    int sent = 0;
    for (int i = 0; i < m_jobs.size(); ++i)
        if (transmit(i))
            ++sent;
    return sent;
}

// ------------------------------------------------------------------ helpers

QString CanSenderModel::formatId(quint32 id, bool extended) const
{
    return QStringLiteral("0x%1")
           .arg(QStringLiteral("%1").arg(id, extended ? 8 : 3, 16, QLatin1Char('0')).toUpper());
}

QString CanSenderModel::formatCycle(int cycleMs) const
{
    return cycleMs > 0 ? tr("%1 ms").arg(cycleMs) : tr("Manual");
}

QString CanSenderModel::bytesToHex(const QVariantList &bytes) const
{
    QStringList out;
    out.reserve(bytes.size());
    for (const QVariant &value : bytes) {
        out << QStringLiteral("%1").arg(value.toUInt() & 0xFF, 2, 16, QLatin1Char('0')).toUpper();
    }
    return out.join(QLatin1Char(' '));
}

QString CanSenderModel::encodePayload(int messageIndex, const QVariantMap &values) const
{
    if (!m_dbcModel)
        return {};

    DbcMessageObject *message = m_dbcModel->message(messageIndex);
    DbcSignalListModel *signalList = m_dbcModel->signalsOf(messageIndex);
    if (!message || !signalList)
        return {};

    const int dlc = qBound(0, message->dlc(), 64);
    std::array<quint8, 64> payload {};

    for (int s = 0; s < signalList->count(); ++s) {
        const DbcSignalObject *signalObject = signalList->get(s);
        if (!signalObject)
            continue;

        const QVariant supplied = values.value(QString::number(s));
        const double physical = supplied.isValid() ? supplied.toDouble()
                                                   : signalObject->minimum();

        const quint64 raw = CanDecoder::toRaw(physical, signalObject->bitLength(),
                                              signalObject->valueType() == 1,
                                              signalObject->factor(), signalObject->offset());
        CanDecoder::insertRaw(payload.data(), dlc, signalObject->startBit(),
                              signalObject->bitLength(), signalObject->byteOrder() != 0, raw);
    }

    return payloadText(payload, dlc);
}

int CanSenderModel::parseHexPayload(const QString &text, std::array<quint8, 64> &out)
{
    const QString compact = QString(text).remove(QLatin1Char(' '))
                                         .remove(QLatin1Char(':'))
                                         .remove(QLatin1Char('-'));
    int written = 0;
    for (int i = 0; i + 1 < compact.size() && written < int(out.size()); i += 2) {
        bool ok = false;
        const quint8 byte = quint8(compact.mid(i, 2).toUInt(&ok, 16));
        if (!ok)
            break;
        out[size_t(written++)] = byte;
    }
    return written;
}

QString CanSenderModel::payloadText(const std::array<quint8, 64> &payload, int dlc)
{
    QString out;
    out.reserve(dlc * 3);
    for (int i = 0; i < dlc && i < int(payload.size()); ++i) {
        if (i > 0)
            out += QLatin1Char(' ');
        out += QStringLiteral("%1").arg(payload[size_t(i)], 2, 16, QLatin1Char('0')).toUpper();
    }
    return out;
}

void CanSenderModel::setLastError(const QString &message)
{
    if (m_lastError == message)
        return;
    m_lastError = message;
    if (!message.isEmpty())
        qWarning("CanSenderModel: %s", qPrintable(message));
    emit lastErrorChanged();
}
