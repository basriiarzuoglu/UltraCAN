#include "DbcModel.h"

#include "dbc/ValueFormat.h"

#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QVariantMap>

#include <cmath>

#include <utility>

// ============================================================ DbcSignalObject

void DbcSignalObject::setData(const DbcSignalData &data)
{
    m_data = data;
    // A wholesale replacement: tell QML every property may have moved.
    emit nameChanged();
    emit startBitChanged();
    emit bitLengthChanged();
    emit byteOrderChanged();
    emit valueTypeChanged();
    emit factorChanged();
    emit offsetChanged();
    emit minimumChanged();
    emit maximumChanged();
    emit unitChanged();
    emit receiverChanged();
    emit commentChanged();
    emit valueDescriptionsChanged();
    emit edited();
}

#define DBC_SET(member, value, signalName)      \
    do {                                        \
        if (m_data.member == (value))           \
            return;                             \
        m_data.member = (value);                \
        emit signalName();                      \
        emit edited();                          \
    } while (false)

void DbcSignalObject::setName(const QString &value)   { DBC_SET(name, value, nameChanged); }
void DbcSignalObject::setStartBit(int value)          { DBC_SET(startBit, value, startBitChanged); }
void DbcSignalObject::setBitLength(int value)         { DBC_SET(bitLength, value, bitLengthChanged); }
void DbcSignalObject::setByteOrder(int value)         { DBC_SET(byteOrder, value, byteOrderChanged); }
void DbcSignalObject::setValueType(int value)         { DBC_SET(valueType, value, valueTypeChanged); }
void DbcSignalObject::setFactor(double value)         { DBC_SET(factor, value, factorChanged); }
void DbcSignalObject::setOffset(double value)         { DBC_SET(offset, value, offsetChanged); }
void DbcSignalObject::setMinimum(double value)        { DBC_SET(minimum, value, minimumChanged); }
void DbcSignalObject::setMaximum(double value)        { DBC_SET(maximum, value, maximumChanged); }
void DbcSignalObject::setUnit(const QString &value)   { DBC_SET(unit, value, unitChanged); }
void DbcSignalObject::setComment(const QString &value){ DBC_SET(comment, value, commentChanged); }

#undef DBC_SET

void DbcSignalObject::setReceiver(const QString &value)
{
    static const QRegularExpression separator(QStringLiteral("\\s*,\\s*"));
    const QStringList receivers = value.split(separator, Qt::SkipEmptyParts);
    if (receivers == m_data.receivers)
        return;
    m_data.receivers = receivers;
    emit receiverChanged();
    emit edited();
}

QVariantList DbcSignalObject::valueDescriptions() const
{
    QVariantList out;
    out.reserve(m_data.valueDescriptions.size());
    for (auto it = m_data.valueDescriptions.constBegin();
         it != m_data.valueDescriptions.constEnd(); ++it) {
        out.append(QVariantMap {
            { QStringLiteral("value"),       it.key()   },
            { QStringLiteral("description"), it.value() }
        });
    }
    return out;                       // QMap iterates in key order - already sorted
}

void DbcSignalObject::setValueDescriptions(const QVariantList &entries)
{
    QMap<int, QString> table;
    for (const QVariant &entry : entries) {
        bool ok = false;
        int value = 0;
        QString description;

        // Probe by CONTENT, not by canConvert<>(): QVariant reports several
        // container conversions as possible and then hands back an empty one,
        // so asking "could this be a map?" silently eats the array form.
        const QVariantMap map = entry.toMap();
        if (map.contains(QStringLiteral("value"))) {
            value = map.value(QStringLiteral("value")).toInt(&ok);
            description = map.value(QStringLiteral("description")).toString();
        } else {
            const QVariantList pair = entry.toList();
            if (pair.size() >= 2) {
                value = pair.at(0).toInt(&ok);
                description = pair.at(1).toString();
            }
        }
        // A row the user started and never filled in is not an error; it just
        // does not become an entry. Silently dropping it is what lets the
        // dialog keep a blank row on screen without writing garbage.
        if (ok)
            table.insert(value, description);
    }

    if (table == m_data.valueDescriptions)
        return;
    m_data.valueDescriptions = table;
    emit valueDescriptionsChanged();
    emit edited();
}

bool DbcSignalObject::setValueDescription(int value, const QString &description, bool replace)
{
    if (!replace && m_data.valueDescriptions.contains(value))
        return false;
    if (m_data.valueDescriptions.value(value) == description
        && m_data.valueDescriptions.contains(value)) {
        return true;                  // already exactly this - not an edit
    }
    m_data.valueDescriptions.insert(value, description);
    emit valueDescriptionsChanged();
    emit edited();
    return true;
}

void DbcSignalObject::removeValueDescription(int value)
{
    if (m_data.valueDescriptions.remove(value) == 0)
        return;
    emit valueDescriptionsChanged();
    emit edited();
}

void DbcSignalObject::clearValueDescriptions()
{
    if (m_data.valueDescriptions.isEmpty())
        return;
    m_data.valueDescriptions.clear();
    emit valueDescriptionsChanged();
    emit edited();
}

QString DbcSignalObject::describeValue(double physical) const
{
    // The scaling inversion and the exact-integer test used to be written out
    // here. They are now in ValueFormat.h, shared with CanDecoder - a second
    // copy of "which code is this?" is how the analyzer and the editor start
    // disagreeing about the same signal.
    return UltraCAN::describeSignalValue(physical, m_data.factor, m_data.offset,
                                       m_data.valueDescriptions);
}

QString DbcSignalObject::formattedValue(double physical) const
{
    return UltraCAN::formatSignalValue(physical, m_data.factor, m_data.offset,
                                     m_data.valueDescriptions);
}

namespace {

/*! Raw value range for a signal of \a bits width. Returned as doubles because
    a 64-bit unsigned maximum does not fit a qint64. */
void rawRange(int bits, bool isSigned, double *lo, double *hi)
{
    const int width = qBound(1, bits, 64);
    if (isSigned) {
        *lo = -std::pow(2.0, width - 1);
        *hi =  std::pow(2.0, width - 1) - 1.0;
    } else {
        *lo = 0.0;
        *hi =  std::pow(2.0, width) - 1.0;
    }
}

} // namespace

double DbcSignalObject::autoMinimum() const
{
    double lo = 0.0;
    double hi = 0.0;
    rawRange(m_data.bitLength, m_data.valueType == 1, &lo, &hi);
    const double a = lo * m_data.factor + m_data.offset;
    const double b = hi * m_data.factor + m_data.offset;
    return qMin(a, b);            // a negative factor swaps the ends
}

double DbcSignalObject::autoMaximum() const
{
    double lo = 0.0;
    double hi = 0.0;
    rawRange(m_data.bitLength, m_data.valueType == 1, &lo, &hi);
    const double a = lo * m_data.factor + m_data.offset;
    const double b = hi * m_data.factor + m_data.offset;
    return qMax(a, b);
}

// ========================================================= DbcSignalListModel

DbcSignalListModel::DbcSignalListModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int DbcSignalListModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_signals.size();
}

QHash<int, QByteArray> DbcSignalListModel::roleNames() const
{
    // Exactly the names DbcEditorView / DbcExplorer read.
    return {
        { NameRole,      QByteArrayLiteral("name")      },
        { StartBitRole,  QByteArrayLiteral("startBit")  },
        { BitLengthRole, QByteArrayLiteral("bitLength") },
        { ByteOrderRole, QByteArrayLiteral("byteOrder") },
        { ValueTypeRole, QByteArrayLiteral("valueType") },
        { FactorRole,    QByteArrayLiteral("factor")    },
        { OffsetRole,    QByteArrayLiteral("offset")    },
        { MinimumRole,   QByteArrayLiteral("minimum")   },
        { MaximumRole,   QByteArrayLiteral("maximum")   },
        { UnitRole,      QByteArrayLiteral("unit")      },
        { ReceiverRole,  QByteArrayLiteral("receiver")  },
        { CommentRole,   QByteArrayLiteral("comment")   },
        { ValueDescriptionsRole, QByteArrayLiteral("valueDescriptions") }
    };
}

QVariant DbcSignalListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_signals.size())
        return {};

    // Roles and properties carry the same names, so one lookup serves both.
    const QByteArray property = roleNames().value(role);
    if (property.isEmpty())
        return {};
    return m_signals.at(index.row())->property(property.constData());
}

DbcSignalObject *DbcSignalListModel::get(int row) const
{
    return (row >= 0 && row < m_signals.size()) ? m_signals.at(row) : nullptr;
}

void DbcSignalListModel::setProperty(int row, const QString &role, const QVariant &value)
{
    DbcSignalObject *object = get(row);
    if (!object)
        return;
    object->QObject::setProperty(role.toUtf8().constData(), value);
}

void DbcSignalListModel::connectSignalObject(DbcSignalObject *object)
{
    connect(object, &DbcSignalObject::edited, this, [this, object] {
        if (m_resetting)
            return;                       // a reset already invalidates every row
        const int row = m_signals.indexOf(object);
        if (row >= 0)
            emit dataChanged(index(row), index(row));
        emit edited();
    });
}

void DbcSignalListModel::applyData(const QVector<DbcSignalData> &data)
{
    m_resetting = true;
    beginResetModel();

    // Reuse the existing objects; only the surplus is destroyed. Anything QML
    // still holds a reference to therefore stays valid across a reload.
    while (m_signals.size() > data.size()) {
        DbcSignalObject *victim = m_signals.takeLast();
        victim->deleteLater();
    }
    while (m_signals.size() < data.size()) {
        auto *object = new DbcSignalObject(this);
        connectSignalObject(object);
        m_signals.append(object);
    }
    for (int i = 0; i < data.size(); ++i)
        m_signals[i]->setData(data.at(i));

    endResetModel();
    m_resetting = false;
    emit countChanged();
}

QVector<DbcSignalData> DbcSignalListModel::toData() const
{
    QVector<DbcSignalData> out;
    out.reserve(m_signals.size());
    for (const DbcSignalObject *object : m_signals)
        out.append(object->data());
    return out;
}

void DbcSignalListModel::appendSignal(const DbcSignalData &data)
{
    beginInsertRows(QModelIndex(), m_signals.size(), m_signals.size());
    auto *object = new DbcSignalObject(this);
    object->setData(data);
    connectSignalObject(object);
    m_signals.append(object);
    endInsertRows();

    emit countChanged();
    emit edited();
}

void DbcSignalListModel::removeSignal(int row)
{
    if (row < 0 || row >= m_signals.size())
        return;

    beginRemoveRows(QModelIndex(), row, row);
    m_signals.takeAt(row)->deleteLater();
    endRemoveRows();

    emit countChanged();
    emit edited();
}

// ========================================================= DbcMessageObject

DbcMessageObject::DbcMessageObject(QObject *parent)
    : QObject(parent)
    , m_signalList(new DbcSignalListModel(this))
{
    connect(m_signalList, &DbcSignalListModel::edited, this, &DbcMessageObject::edited);
}

#define DBC_SET(member, value, signalName)      \
    do {                                        \
        if (member == (value))                  \
            return;                             \
        member = (value);                       \
        emit signalName();                      \
        emit edited();                          \
    } while (false)

void DbcMessageObject::setName(const QString &value)        { DBC_SET(m_name, value, nameChanged); }
void DbcMessageObject::setCanId(quint32 value)              { DBC_SET(m_canId, value, canIdChanged); }
void DbcMessageObject::setExtended(bool value)              { DBC_SET(m_extended, value, extendedChanged); }
void DbcMessageObject::setDlc(int value)                    { DBC_SET(m_dlc, value, dlcChanged); }
void DbcMessageObject::setTransmitter(const QString &value) { DBC_SET(m_transmitter, value, transmitterChanged); }
void DbcMessageObject::setCycleMs(int value)                { DBC_SET(m_cycleMs, value, cycleMsChanged); }
void DbcMessageObject::setComment(const QString &value)     { DBC_SET(m_comment, value, commentChanged); }

#undef DBC_SET

void DbcMessageObject::setExpanded(bool value)
{
    // Pure UI state: it must NOT mark the document modified.
    if (m_expanded == value)
        return;
    m_expanded = value;
    emit expandedChanged();
}

void DbcMessageObject::applyData(const DbcMessageData &data)
{
    m_name = data.name;
    m_canId = data.id;
    m_extended = data.extended;
    m_dlc = data.dlc;
    m_transmitter = data.transmitter;
    m_cycleMs = data.cycleMs;
    m_comment = data.comment;

    emit nameChanged();
    emit canIdChanged();
    emit extendedChanged();
    emit dlcChanged();
    emit transmitterChanged();
    emit cycleMsChanged();
    emit commentChanged();

    m_signalList->applyData(data.signalList);
}

DbcMessageData DbcMessageObject::toData() const
{
    DbcMessageData data;
    data.name = m_name;
    data.id = m_canId;
    data.extended = m_extended;
    data.dlc = m_dlc;
    data.transmitter = m_transmitter;
    data.cycleMs = m_cycleMs;
    data.comment = m_comment;
    data.signalList = m_signalList->toData();
    return data;
}

// ================================================================== DbcModel

DbcModel *DbcModel::s_instance = nullptr;

DbcModel::DbcModel(QObject *parent)
    : QAbstractListModel(parent)
{
    if (!s_instance) {
        s_instance = this;
    } else {
        qCritical("DbcModel: a SECOND instance was created (%p; first was %p).",
                  static_cast<void *>(this), static_cast<void *>(s_instance));
    }

    // NO loadDemo() here.
    //
    // It used to run, "so the editor is not empty before a file is opened",
    // and it is the second half of why the editor came up wrong. The demo is
    // three messages and six signals of invented powertrain content, and it
    // was indistinguishable from a real document: the grid was populated, the
    // decode table was populated, and an edit made "to a blank file" was an
    // edit to demo_powertrain.dbc. Disabling the start-up restore of the last
    // .dbc was pointless while this still ran - the editor was never empty.
    //
    // It also made the stale-key window WORSE rather than better: with a demo
    // table already loaded, keys resolved during that window pointed at real
    // entries belonging to different signals, so the live column showed wrong
    // numbers instead of showing nothing.
    //
    // loadDemo() stays Q_INVOKABLE - it is genuinely useful from QML for a
    // "show me an example" action - it just is not what the application opens
    // with.
}

DbcModel *DbcModel::instance()
{
    return s_instance;
}


int DbcModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_messages.size();
}

QHash<int, QByteArray> DbcModel::roleNames() const
{
    return {
        { NameRole,        QByteArrayLiteral("name")        },
        { CanIdRole,       QByteArrayLiteral("canId")       },
        { ExtendedRole,    QByteArrayLiteral("extended")    },
        { DlcRole,         QByteArrayLiteral("dlc")         },
        { TransmitterRole, QByteArrayLiteral("transmitter") },
        { CycleMsRole,     QByteArrayLiteral("cycleMs")     },
        { CommentRole,     QByteArrayLiteral("comment")     },
        { ExpandedRole,    QByteArrayLiteral("expanded")    },
        { SignalListRole,  QByteArrayLiteral("signalList")  }
    };
}

QVariant DbcModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_messages.size())
        return {};

    DbcMessageObject *message = m_messages.at(index.row());
    if (role == SignalListRole)
        return QVariant::fromValue(message->signalList());   // usable as a Repeater model

    const QByteArray property = roleNames().value(role);
    if (property.isEmpty())
        return {};
    return message->property(property.constData());
}

// ----------------------------------------------------- ListModel look-alike

DbcMessageObject *DbcModel::get(int row) const
{
    return (row >= 0 && row < m_messages.size()) ? m_messages.at(row) : nullptr;
}

void DbcModel::setProperty(int row, const QString &role, const QVariant &value)
{
    DbcMessageObject *object = get(row);
    if (!object)
        return;
    object->QObject::setProperty(role.toUtf8().constData(), value);
}

// ------------------------------------------------------------- the old API

DbcMessageObject *DbcModel::message(int index) const
{
    return get(index);
}

DbcSignalListModel *DbcModel::signalsOf(int messageIndex) const
{
    DbcMessageObject *object = get(messageIndex);
    return object ? object->signalList() : nullptr;
}

DbcSignalObject *DbcModel::signalAt(int messageIndex, int signalIndex) const
{
    DbcSignalListModel *list = signalsOf(messageIndex);
    return list ? list->get(signalIndex) : nullptr;
}

void DbcModel::setMessageProperty(int messageIndex, const QString &role, const QVariant &value)
{
    setProperty(messageIndex, role, value);
}

void DbcModel::setSignalProperty(int messageIndex, int signalIndex,
                                 const QString &role, const QVariant &value)
{
    if (DbcSignalListModel *list = signalsOf(messageIndex))
        list->setProperty(signalIndex, role, value);
}

int DbcModel::addMessage()
{
    DbcMessageData data;
    data.name = QStringLiteral("NewMessage_%1").arg(m_messages.size() + 1);
    data.id = quint32(0x100 + m_messages.size() * 0x10);
    data.dlc = 8;
    data.transmitter = m_nodeNames.value(0);
    data.cycleMs = 100;

    beginInsertRows(QModelIndex(), m_messages.size(), m_messages.size());
    auto *object = new DbcMessageObject(this);
    object->applyData(data);
    connectMessageObject(object, m_messages.size());
    m_messages.append(object);
    endInsertRows();

    setModified(true);
    emit countChanged();
    emit databaseChanged();
    return m_messages.size() - 1;
}

int DbcModel::addSignal(int messageIndex)
{
    DbcMessageObject *message = get(messageIndex);
    if (!message)
        return -1;

    DbcSignalListModel *list = message->signalList();

    int nextBit = 0;
    for (int i = 0; i < list->count(); ++i) {
        const DbcSignalObject *existing = list->get(i);
        nextBit = qMax(nextBit, existing->startBit() + existing->bitLength());
    }

    DbcSignalData data;
    data.name = QStringLiteral("NewSignal_%1").arg(list->count() + 1);
    data.startBit = qMin(nextBit, message->dlc() * 8 - 1);
    data.bitLength = 8;
    data.byteOrder = byteOrderIntel();
    data.maximum = 255.0;
    if (!m_nodeNames.isEmpty())
        data.receivers << m_nodeNames.first();

    list->appendSignal(data);
    message->setExpanded(true);
    setModified(true);

    const QModelIndex idx = index(messageIndex);
    emit dataChanged(idx, idx, { SignalListRole });
    emit databaseChanged();
    return list->count() - 1;
}

void DbcModel::removeMessage(int messageIndex)
{
    if (messageIndex < 0 || messageIndex >= m_messages.size())
        return;

    beginRemoveRows(QModelIndex(), messageIndex, messageIndex);
    m_messages.takeAt(messageIndex)->deleteLater();
    endRemoveRows();

    setModified(true);
    emit countChanged();
    emit databaseChanged();
}

void DbcModel::removeSignal(int messageIndex, int signalIndex)
{
    if (DbcSignalListModel *list = signalsOf(messageIndex)) {
        list->removeSignal(signalIndex);
        setModified(true);
        emit databaseChanged();
    }
}

void DbcModel::toggleExpanded(int messageIndex)
{
    if (DbcMessageObject *object = get(messageIndex))
        object->setExpanded(!object->expanded());
}

void DbcModel::setExpandedAll(bool expanded)
{
    for (DbcMessageObject *object : std::as_const(m_messages))
        object->setExpanded(expanded);
}

QString DbcModel::formatCanId(quint32 id, bool extended) const
{
    return QStringLiteral("0x%1")
           .arg(QStringLiteral("%1").arg(id, extended ? 8 : 3, 16, QLatin1Char('0')).toUpper());
}

QStringList DbcModel::byteOrderNames() const
{
    return { tr("Motorola (BE)"), tr("Intel (LE)") };
}

QStringList DbcModel::valueTypeNames() const
{
    return { tr("Unsigned"), tr("Signed") };
}

// ------------------------------------------------------------------ file I/O

bool DbcModel::loadDbc(const QUrl &fileUrl)
{
    const QString path = fileUrl.isLocalFile() ? fileUrl.toLocalFile() : fileUrl.toString();

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

    setLastError(QString());
    applyDatabase(parsed);
    setModified(false);

    // Recorded only now, on the success path: a file that failed to parse must
    // never become the one the next launch tries to reopen.
    if (m_sourceUrl != fileUrl) {
        m_sourceUrl = fileUrl;
        emit sourceUrlChanged();
    }
    return true;
}

bool DbcModel::saveDbc(const QUrl &fileUrl)
{
    const QString path = fileUrl.isLocalFile() ? fileUrl.toLocalFile() : fileUrl.toString();

    // QSaveFile: the old file survives if writing fails half way.
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        setLastError(tr("Cannot write %1: %2").arg(path, file.errorString()));
        return false;
    }

    file.write(DbcParser::serialize(database()).toUtf8());
    if (!file.commit()) {
        setLastError(tr("Cannot write %1: %2").arg(path, file.errorString()));
        return false;
    }

    setLastError(QString());
    setDatabaseName(QFileInfo(path).fileName());
    setModified(false);

    // "Save As" moves the database: the new file is the one to reopen.
    if (m_sourceUrl != fileUrl) {
        m_sourceUrl = fileUrl;
        emit sourceUrlChanged();
    }
    return true;
}

void DbcModel::loadDemo()
{
    DbcDatabase demo;
    demo.name = QStringLiteral("demo_powertrain.dbc");
    demo.nodes = { QStringLiteral("ECU"), QStringLiteral("BMS"), QStringLiteral("VCU"),
                   QStringLiteral("Dashboard"), QStringLiteral("Charger") };

    auto makeSignal = [](const QString &name, int startBit, int bitLength, int byteOrder,
                         int valueType, double factor, double offset, double minimum,
                         double maximum, const QString &unit, const QString &receiver,
                         const QString &comment) {
        DbcSignalData s;
        s.name = name;
        s.startBit = startBit;
        s.bitLength = bitLength;
        s.byteOrder = byteOrder;
        s.valueType = valueType;
        s.factor = factor;
        s.offset = offset;
        s.minimum = minimum;
        s.maximum = maximum;
        s.unit = unit;
        s.receivers = QStringList { receiver };
        s.comment = comment;
        return s;
    };

    DbcMessageData engine;
    engine.id = 0x100;
    engine.name = QStringLiteral("EngineData");
    engine.dlc = 8;
    engine.transmitter = QStringLiteral("ECU");
    engine.cycleMs = 10;
    engine.comment = QStringLiteral("Primary engine status frame");
    engine.signalList = {
        makeSignal(QStringLiteral("EngineSpeed"), 0, 16, 1, 0, 0.125, 0, 0, 8000,
                   QStringLiteral("rpm"), QStringLiteral("Dashboard"),
                   QStringLiteral("Crankshaft speed")),
        makeSignal(QStringLiteral("CoolantTemp"), 16, 8, 1, 1, 1, -40, -40, 215,
                   QStringLiteral("degC"), QStringLiteral("Dashboard"),
                   QStringLiteral("Coolant temperature")),
        makeSignal(QStringLiteral("ThrottlePos"), 24, 8, 1, 0, 0.4, 0, 0, 100,
                   QStringLiteral("%"), QStringLiteral("VCU"),
                   QStringLiteral("Pedal position"))
    };

    DbcMessageData battery;
    battery.id = 0x200;
    battery.name = QStringLiteral("BatteryStatus");
    battery.dlc = 8;
    battery.transmitter = QStringLiteral("BMS");
    battery.cycleMs = 100;
    battery.comment = QStringLiteral("High voltage pack summary");
    battery.signalList = {
        makeSignal(QStringLiteral("PackVoltage"), 0, 16, 0, 0, 0.01, 0, 0, 650,
                   QStringLiteral("V"), QStringLiteral("VCU"), QString()),
        makeSignal(QStringLiteral("PackCurrent"), 16, 16, 0, 1, 0.1, -3200, -3200, 3200,
                   QStringLiteral("A"), QStringLiteral("VCU"),
                   QStringLiteral("Positive = discharge"))
    };

    DbcMessageData command;
    command.id = 0x300;
    command.name = QStringLiteral("VehicleCommand");
    command.dlc = 4;
    command.transmitter = QStringLiteral("VCU");
    command.cycleMs = 20;
    command.signalList = {
        makeSignal(QStringLiteral("TorqueRequest"), 0, 12, 1, 1, 0.5, -1024, -1024, 1023.5,
                   QStringLiteral("Nm"), QStringLiteral("ECU"), QString())
    };

    demo.messages = { engine, battery, command };
    applyDatabase(demo);
    setModified(false);
}

// ------------------------------------------------------------------ internals

void DbcModel::applyDatabase(const DbcDatabase &database)
{
    m_resetting = true;
    beginResetModel();

    // Reuse message objects across loads; only the surplus is discarded. A QML
    // binding that captured message(2) therefore keeps pointing at a live
    // object instead of a deleted one.
    while (m_messages.size() > database.messages.size()) {
        DbcMessageObject *victim = m_messages.takeLast();
        victim->deleteLater();
    }
    while (m_messages.size() < database.messages.size()) {
        auto *object = new DbcMessageObject(this);
        connectMessageObject(object, m_messages.size());
        m_messages.append(object);
    }
    for (int i = 0; i < database.messages.size(); ++i)
        m_messages[i]->applyData(database.messages.at(i));

    endResetModel();
    m_resetting = false;

    m_warnings = database.warnings;
    m_nodeNames = database.nodes;
    if (m_nodeNames.isEmpty()) {
        for (const DbcMessageData &message : database.messages)
            if (!message.transmitter.isEmpty() && !m_nodeNames.contains(message.transmitter))
                m_nodeNames << message.transmitter;
    }

    setDatabaseName(database.name);
    emit nodeNamesChanged();
    emit countChanged();
    emit loaded();
    emit databaseChanged();
}

void DbcModel::connectMessageObject(DbcMessageObject *object, int row)
{
    Q_UNUSED(row)
    connect(object, &DbcMessageObject::edited, this, [this, object] {
        if (m_resetting)
            return;                       // a reset already invalidates every row
        const int index = m_messages.indexOf(object);
        if (index >= 0) {
            const QModelIndex idx = this->index(index);
            emit dataChanged(idx, idx);
        }
        setModified(true);
        emit databaseChanged();          // a changed factor or start bit re-decodes
    });
    connect(object, &DbcMessageObject::expandedChanged, this, [this, object] {
        if (m_resetting)
            return;
        const int index = m_messages.indexOf(object);
        if (index >= 0) {
            const QModelIndex idx = this->index(index);
            emit dataChanged(idx, idx, { ExpandedRole });
        }
    });
}

DbcDatabase DbcModel::database() const
{
    DbcDatabase out;
    out.name = m_databaseName;
    out.nodes = m_nodeNames;
    out.messages.reserve(m_messages.size());
    for (const DbcMessageObject *object : m_messages)
        out.messages.append(object->toData());
    return out;
}

void DbcModel::setDatabaseName(const QString &name)
{
    if (m_databaseName == name)
        return;
    m_databaseName = name;
    emit databaseNameChanged();
}

void DbcModel::setModified(bool modified)
{
    if (m_modified == modified)
        return;
    m_modified = modified;
    emit modifiedChanged();
}

void DbcModel::setLastError(const QString &message)
{
    if (m_lastError == message)
        return;
    m_lastError = message;
    if (!message.isEmpty())
        qWarning("DBC: %s", qPrintable(message));
    emit lastErrorChanged();
}
