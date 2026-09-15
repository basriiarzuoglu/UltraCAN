#include "UdsClient.h"

#include "IsoTpLayer.h"
#include "core/CanBusManager.h"

#include <QRegularExpression>
#include <QTimer>

UdsClient::UdsClient(QObject *parent)
    : QObject(parent)
    , m_tp(new IsoTpLayer(this))
{
    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);
    connect(m_timer, &QTimer::timeout, this, &UdsClient::onTimeout);

    m_tp->setAddressing(m_txId, m_rxId, m_extendedIds);
    connect(m_tp, &IsoTpLayer::messageReceived, this, &UdsClient::onMessageReceived);
    connect(m_tp, &IsoTpLayer::errorOccurred, this, &UdsClient::onTransportError);
}

UdsClient::~UdsClient() = default;

void UdsClient::setBus(CanBusManager *bus)
{
    if (m_bus == bus)
        return;
    if (m_bus)
        disconnect(m_bus, nullptr, this, nullptr);

    m_bus = bus;
    m_tp->setBus(bus);

    if (m_bus) {
        connect(m_bus, &CanBusManager::connectionChanged, this, [this] {
            // A bus that goes away mid-request will never answer it.
            if (!busReady() && busy())
                abort();
            emit busReadyChanged();
        });
    }
    emit busReadyChanged();
}

bool UdsClient::busReady() const
{
    return m_bus && m_bus->isConnected();
}

void UdsClient::setTxId(quint32 id)
{
    if (m_txId == id)
        return;
    m_txId = id;
    m_tp->setAddressing(m_txId, m_rxId, m_extendedIds);
    emit addressingChanged();
}

void UdsClient::setRxId(quint32 id)
{
    if (m_rxId == id)
        return;
    m_rxId = id;
    m_tp->setAddressing(m_txId, m_rxId, m_extendedIds);
    emit addressingChanged();
}

void UdsClient::setExtendedIds(bool extended)
{
    if (m_extendedIds == extended)
        return;
    m_extendedIds = extended;
    m_tp->setAddressing(m_txId, m_rxId, m_extendedIds);
    emit addressingChanged();
}

// ------------------------------------------------------------------ requests

bool UdsClient::sendRequest(const QByteArray &payload)
{
    if (payload.isEmpty()) {
        setLastError(tr("Empty request."));
        return false;
    }
    if (busy()) {
        setLastError(tr("Still waiting for the answer to service 0x%1.")
                     .arg(m_pendingService, 2, 16, QLatin1Char('0')).toUpper());
        return false;
    }
    if (!busReady()) {
        setLastError(tr("No CAN interface is connected."));
        return false;
    }

    if (!m_tp->send(payload))
        return false;                 // the transport already reported why

    m_pendingService = quint8(payload.at(0));
    setLastError(QString());
    emit requestSent(payload);
    emit busyChanged();
    m_timer->start(DefaultTimeoutMs);
    return true;
}

bool UdsClient::requestReadDtc(int statusMask)
{
    QByteArray request;
    request.append(char(ReadDtcInformation));
    request.append(char(0x02));                       // reportDTCByStatusMask
    request.append(char(statusMask & 0xFF));
    return sendRequest(request);
}

bool UdsClient::clearDtc(quint32 groupOfDtc)
{
    QByteArray request;
    request.append(char(ClearDiagnosticInfo));
    request.append(char((groupOfDtc >> 16) & 0xFF));
    request.append(char((groupOfDtc >> 8) & 0xFF));
    request.append(char(groupOfDtc & 0xFF));
    return sendRequest(request);
}

bool UdsClient::readDataByIdentifier(int identifier)
{
    QByteArray request;
    request.append(char(ReadDataByIdentifier));
    request.append(char((identifier >> 8) & 0xFF));
    request.append(char(identifier & 0xFF));
    return sendRequest(request);
}

bool UdsClient::diagnosticSessionControl(int session)
{
    QByteArray request;
    request.append(char(DiagnosticSessionControl));
    request.append(char(session & 0xFF));
    return sendRequest(request);
}

bool UdsClient::testerPresent()
{
    QByteArray request;
    request.append(char(TesterPresent));
    request.append(char(0x00));                       // zeroSubFunction
    return sendRequest(request);
}

bool UdsClient::sendHexRequest(const QString &hexText)
{
    // Accept "22 01 00", "2201 00", "0x22 0x01" and "22-01-00" alike: the
    // point of the console is to paste bytes from a datasheet, and every
    // datasheet writes them differently.
    static const QRegularExpression separators(QStringLiteral("(0[xX])|[\\s,;:_-]+"));
    const QString compact = QString(hexText).remove(separators);

    if (compact.isEmpty()) {
        setLastError(tr("Type some hex bytes first, for example 22 01 00."));
        return false;
    }
    if (compact.size() % 2 != 0) {
        setLastError(tr("“%1” is an odd number of hex digits - a byte needs two.")
                     .arg(hexText.trimmed()));
        return false;
    }
    static const QRegularExpression onlyHex(QStringLiteral("\\A[0-9a-fA-F]+\\z"));
    if (!onlyHex.match(compact).hasMatch()) {
        setLastError(tr("“%1” contains something that is not a hex digit.")
                     .arg(hexText.trimmed()));
        return false;
    }

    return sendRequest(QByteArray::fromHex(compact.toLatin1()));
}

void UdsClient::abort()
{
    m_timer->stop();
    m_tp->reset();
    if (m_pendingService != 0) {
        m_pendingService = 0;
        emit busyChanged();
    }
}

// ----------------------------------------------------------------- responses

void UdsClient::onMessageReceived(const QByteArray &data)
{
    if (data.isEmpty())
        return;

    const quint8 first = quint8(data.at(0));

    // -- negative response ----------------------------------------------------
    if (first == NegativeResponse) {
        if (data.size() < 3) {
            setLastError(tr("Truncated negative response."));
            finish();
            return;
        }
        const quint8 service = quint8(data.at(1));
        const quint8 nrc = quint8(data.at(2));

        if (nrc == ResponsePending) {
            // NOT a failure - the ECU is asking for more time. Keep the
            // request pending and give it the longer P2* budget.
            emit responsePending(service);
            m_timer->start(PendingTimeoutMs);
            return;
        }

        const QString text = describeNrc(nrc);
        setLastError(tr("Service 0x%1 rejected: %2")
                     .arg(service, 2, 16, QLatin1Char('0')).toUpper().arg(text));
        emit negativeResponse(service, nrc, text);
        finish();
        return;
    }

    // -- positive response ----------------------------------------------------
    // A positive response id is the request's service id + 0x40. Anything
    // else on our Rx id belongs to a different exchange; report it rather
    // than silently decoding it as ours.
    const quint8 answered = quint8(first - 0x40);
    if (m_pendingService != 0 && answered != m_pendingService) {
        setLastError(tr("Response 0x%1 does not answer service 0x%2.")
                     .arg(first, 2, 16, QLatin1Char('0'))
                     .arg(m_pendingService, 2, 16, QLatin1Char('0')).toUpper());
    }

    emit responseReceived(data);

    if (answered == ReadDtcInformation)
        emit dtcRead(decodeDtcResponse(data));

    finish();
}

QStringList UdsClient::decodeDtcResponse(const QByteArray &data) const
{
    // 0x59 0x02 <statusAvailabilityMask> then 4 bytes per DTC:
    // three DTC bytes and one status byte.
    QStringList out;
    if (data.size() < 3)
        return out;
    if (quint8(data.at(1)) != 0x02)
        return out;                    // a sub-function this decoder does not model

    for (int i = 3; i + 3 < data.size(); i += 4) {
        const quint8 b0 = quint8(data.at(i));
        const quint8 b1 = quint8(data.at(i + 1));
        const quint8 b2 = quint8(data.at(i + 2));
        const quint8 status = quint8(data.at(i + 3));
        out << QStringLiteral("%1   %2")
               .arg(formatDtc(b0, b1, b2), describeDtcStatus(status));
    }
    return out;
}

void UdsClient::onTransportError(int error, const QString &text)
{
    Q_UNUSED(error)
    setLastError(text);
    emit errorOccurred(text);
    if (busy())
        finish();
}

void UdsClient::onTimeout()
{
    const quint8 service = m_pendingService;
    setLastError(tr("No answer to service 0x%1.")
                 .arg(service, 2, 16, QLatin1Char('0')).toUpper());
    emit errorOccurred(m_lastError);
    m_tp->reset();
    finish();
}

void UdsClient::finish()
{
    m_timer->stop();
    if (m_pendingService != 0) {
        m_pendingService = 0;
        emit busyChanged();
    }
}

void UdsClient::setLastError(const QString &text)
{
    if (m_lastError == text)
        return;
    m_lastError = text;
    emit lastErrorChanged();
}

// ------------------------------------------------------------------ decoding

QString UdsClient::formatDtc(quint8 high, quint8 middle, quint8 low)
{
    // ISO 15031-6 / SAE J2012 packing of the first two bytes:
    //   bits 15-14  system   00 P (powertrain) 01 C (chassis)
    //                        10 B (body)       11 U (network)
    //   bits 13-12  first digit, 0-3
    //   bits 11-8   second digit, hex
    //   bits  7-0   third and fourth digits, hex
    // The third byte is the UDS failure type, shown after a dash the way
    // every scan tool prints it.
    static const char kSystem[4] = { 'P', 'C', 'B', 'U' };

    const QChar system = QLatin1Char(kSystem[(high >> 6) & 0x03]);
    const int digit1 = (high >> 4) & 0x03;
    const int digit2 = high & 0x0F;
    const int digit3 = (middle >> 4) & 0x0F;
    const int digit4 = middle & 0x0F;

    return QStringLiteral("%1%2%3%4%5-%6")
            .arg(system)
            .arg(digit1)
            .arg(digit2, 1, 16)
            .arg(digit3, 1, 16)
            .arg(digit4, 1, 16)
            .arg(low, 2, 16, QLatin1Char('0'))
            .toUpper();
}

QString UdsClient::describeDtcStatus(quint8 status)
{
    // ISO 14229-1 Annex D, statusOfDTC.
    QStringList bits;
    if (status & 0x01) bits << QStringLiteral("testFailed");
    if (status & 0x02) bits << QStringLiteral("failedThisCycle");
    if (status & 0x04) bits << QStringLiteral("pending");
    if (status & 0x08) bits << QStringLiteral("confirmed");
    if (status & 0x10) bits << QStringLiteral("notCompletedSinceClear");
    if (status & 0x20) bits << QStringLiteral("failedSinceClear");
    if (status & 0x40) bits << QStringLiteral("notCompletedThisCycle");
    if (status & 0x80) bits << QStringLiteral("warningIndicator");

    if (bits.isEmpty())
        return QStringLiteral("no status bits set");
    return bits.join(QStringLiteral(", "));
}

QString UdsClient::describeNrc(quint8 nrc)
{
    switch (nrc) {
    case 0x10: return tr("general reject");
    case 0x11: return tr("service not supported");
    case 0x12: return tr("sub-function not supported");
    case 0x13: return tr("incorrect message length or invalid format");
    case 0x14: return tr("response too long");
    case 0x21: return tr("busy - repeat the request");
    case 0x22: return tr("conditions not correct");
    case 0x24: return tr("request sequence error");
    case 0x25: return tr("no response from sub-net component");
    case 0x31: return tr("request out of range");
    case 0x33: return tr("security access denied");
    case 0x35: return tr("invalid key");
    case 0x36: return tr("exceeded number of attempts");
    case 0x37: return tr("required time delay not expired");
    case 0x70: return tr("upload/download not accepted");
    case 0x72: return tr("general programming failure");
    case 0x73: return tr("wrong block sequence counter");
    case 0x78: return tr("response pending");
    case 0x7E: return tr("sub-function not supported in active session");
    case 0x7F: return tr("service not supported in active session");
    case 0x81: return tr("rpm too high");
    case 0x82: return tr("rpm too low");
    case 0x83: return tr("engine is running");
    case 0x84: return tr("engine is not running");
    case 0x87: return tr("shifter lever not in park");
    case 0x92: return tr("voltage too high");
    case 0x93: return tr("voltage too low");
    default:   return tr("unknown negative response code");
    }
}

QString UdsClient::toHex(const QByteArray &data)
{
    return QString::fromLatin1(data.toHex(' ')).toUpper();
}
