#include "IsoTpLayer.h"

#include "core/CanBusManager.h"

#include <QTimer>

IsoTpLayer::IsoTpLayer(QObject *parent)
    : QObject(parent)
{
    m_flowControlTimer = new QTimer(this);
    m_flowControlTimer->setSingleShot(true);
    m_flowControlTimer->setInterval(FlowControlTimeoutMs);
    connect(m_flowControlTimer, &QTimer::timeout, this, &IsoTpLayer::onFlowControlTimeout);

    m_consecutiveTimer = new QTimer(this);
    m_consecutiveTimer->setSingleShot(true);
    m_consecutiveTimer->setInterval(ConsecutiveTimeoutMs);
    connect(m_consecutiveTimer, &QTimer::timeout, this, &IsoTpLayer::onConsecutiveTimeout);

    m_stMinTimer = new QTimer(this);
    m_stMinTimer->setSingleShot(true);
    connect(m_stMinTimer, &QTimer::timeout, this, &IsoTpLayer::sendNextConsecutiveFrame);
}

IsoTpLayer::~IsoTpLayer() = default;

void IsoTpLayer::setBus(CanBusManager *bus)
{
    m_bus = bus;
}

void IsoTpLayer::setAddressing(quint32 txId, quint32 rxId, bool extendedIds)
{
    if (m_txId == txId && m_rxId == rxId && m_extendedIds == extendedIds)
        return;

    // Re-addressing mid-exchange would reassemble the new ECU's frames into
    // the old one's buffer. Everything in flight belonged to the old pair.
    reset();

    m_txId = txId;
    m_rxId = rxId;
    m_extendedIds = extendedIds;
}

void IsoTpLayer::setPadding(bool enabled, quint8 value)
{
    m_padding = enabled;
    m_paddingByte = value;
}

void IsoTpLayer::setMaxRxPayload(int bytes)
{
    m_maxRxPayload = qBound(7, bytes, MaxPayload);
}

bool IsoTpLayer::busy() const
{
    return m_txState != TxIdle || m_rxState != RxIdle;
}

// ------------------------------------------------------------------ transmit

bool IsoTpLayer::writeFrame(const QByteArray &pciAndData)
{
    if (!m_bus)
        return false;

    CanFrame frame;
    frame.id = m_txId;
    frame.extended = m_extendedIds;
    frame.direction = CanFrame::Tx;

    const int n = qMin<int>(pciAndData.size(), 8);
    for (int i = 0; i < n; ++i)
        frame.payload[i] = quint8(pciAndData.at(i));

    if (m_padding) {
        for (int i = n; i < 8; ++i)
            frame.payload[i] = m_paddingByte;
        frame.dlc = 8;
    } else {
        frame.dlc = quint8(n);
    }

    return m_bus->sendFrame(frame);
}

bool IsoTpLayer::send(const QByteArray &payload)
{
    if (payload.isEmpty()) {
        emit errorOccurred(PayloadTooLong, tr("Nothing to send."));
        return false;
    }
    if (m_txState != TxIdle) {
        emit errorOccurred(Busy, tr("A message is still being sent."));
        return false;
    }
    if (payload.size() > MaxPayload) {
        emit errorOccurred(PayloadTooLong,
                           tr("%1 bytes exceeds the 4095-byte ISO-TP limit.")
                           .arg(payload.size()));
        return false;
    }

    // -- Single Frame: the whole message fits beside its own length nibble.
    if (payload.size() <= 7) {
        QByteArray sf;
        sf.append(char(payload.size()));
        sf.append(payload);
        if (!writeFrame(sf)) {
            abortTx(TransmitFailed, tr("The bus refused the frame - is an interface connected?"));
            return false;
        }
        emit framePassed(true, QStringLiteral("SF"), sf.toHex(' ').toUpper());
        emit messageSent(payload);
        return true;
    }

    // -- First Frame: 12-bit length, then the first six bytes.
    const int length = payload.size();
    QByteArray ff;
    ff.append(char(0x10 | ((length >> 8) & 0x0F)));
    ff.append(char(length & 0xFF));
    ff.append(payload.left(6));

    if (!writeFrame(ff)) {
        abortTx(TransmitFailed, tr("The bus refused the frame - is an interface connected?"));
        return false;
    }
    emit framePassed(true, QStringLiteral("FF"), ff.toHex(' ').toUpper());

    m_txPayload = payload;
    m_txOffset = 6;
    m_txSeq = 1;
    m_txFramesInBlock = 0;
    m_txState = TxWaitFlowControl;
    m_flowControlTimer->start(FlowControlTimeoutMs);
    return true;
}

void IsoTpLayer::sendNextConsecutiveFrame()
{
    if (m_txState != TxSending)
        return;

    const int remaining = m_txPayload.size() - m_txOffset;
    const int chunk = qMin(7, remaining);

    QByteArray cf;
    cf.append(char(0x20 | (m_txSeq & 0x0F)));
    cf.append(m_txPayload.mid(m_txOffset, chunk));

    if (!writeFrame(cf)) {
        abortTx(TransmitFailed, tr("The bus refused a consecutive frame."));
        return;
    }
    emit framePassed(true, QStringLiteral("CF"), cf.toHex(' ').toUpper());

    m_txOffset += chunk;
    m_txSeq = quint8((m_txSeq + 1) & 0x0F);   // wraps 15 -> 0, NOT 15 -> 1
    ++m_txFramesInBlock;

    if (m_txOffset >= m_txPayload.size()) {
        const QByteArray sent = m_txPayload;
        m_txState = TxIdle;
        m_txPayload.clear();
        m_txOffset = 0;
        emit messageSent(sent);
        return;
    }

    // A block size of zero means "send the rest without asking again".
    if (m_txBlockSize != 0 && m_txFramesInBlock >= int(m_txBlockSize)) {
        m_txFramesInBlock = 0;
        m_txState = TxWaitFlowControl;
        m_flowControlTimer->start(FlowControlTimeoutMs);
        return;
    }

    m_stMinTimer->start(m_txStMinMs);
}

// ------------------------------------------------------------------- receive

void IsoTpLayer::ingest(const QVector<CanFrame> &frames)
{
    // ON THE DEVICE THREAD. Keep this to the one decision that has to happen
    // here, and post nothing when nothing matched - which is the usual case.
    bool queued = false;
    {
        QMutexLocker locker(&m_inboxMutex);
        for (const CanFrame &frame : frames) {
            // Rx only. CanBusManager echoes our OWN transmissions into the
            // trace as Tx, and a loopback id pair would otherwise feed this
            // layer its own First Frame.
            if (frame.direction != CanFrame::Rx)
                continue;
            if (frame.id != m_rxId || frame.extended != m_extendedIds)
                continue;
            m_inbox.append(frame);
            queued = true;
        }
        if (!queued)
            return;
        if (m_drainPending)
            return;               // a drain is already on its way
        m_drainPending = true;
    }

    QMetaObject::invokeMethod(this, &IsoTpLayer::drainInbox, Qt::QueuedConnection);
}

void IsoTpLayer::drainInbox()
{
    QVector<CanFrame> batch;
    {
        QMutexLocker locker(&m_inboxMutex);
        batch.swap(m_inbox);
        m_drainPending = false;
    }
    for (const CanFrame &frame : std::as_const(batch))
        handleFrame(frame);
}

void IsoTpLayer::handleFrame(const CanFrame &frame)
{
    if (frame.dlc < 1)
        return;

    switch (frame.payload[0] >> 4) {
    case 0x0: handleSingleFrame(frame);      break;
    case 0x1: handleFirstFrame(frame);       break;
    case 0x2: handleConsecutiveFrame(frame); break;
    case 0x3: handleFlowControl(frame);      break;
    default:  break;                          // not an ISO-TP PCI; ignore
    }
}

void IsoTpLayer::handleSingleFrame(const CanFrame &frame)
{
    const int length = frame.payload[0] & 0x0F;
    // Length 0 is the CAN FD escape, and a length that runs past the frame is
    // a malformed one. Neither is something to guess at.
    if (length < 1 || length > 7 || length + 1 > int(frame.dlc))
        return;

    QByteArray data;
    data.resize(length);
    for (int i = 0; i < length; ++i)
        data[i] = char(frame.payload[i + 1]);

    // An SF arriving mid-reassembly means the ECU gave up on the old message.
    if (m_rxState != RxIdle)
        abortRx(SequenceNumberMismatch, tr("A new message started before the previous one finished."));

    emit framePassed(false, QStringLiteral("SF"), frameBytes(frame));
    emit messageReceived(data);
}

void IsoTpLayer::handleFirstFrame(const CanFrame &frame)
{
    if (frame.dlc < 8)
        return;                                  // a FF is always eight bytes

    const int length = ((frame.payload[0] & 0x0F) << 8) | frame.payload[1];
    emit framePassed(false, QStringLiteral("FF"), frameBytes(frame));

    if (length <= 7) {
        // Would have fitted in a Single Frame; per ISO this FF is invalid.
        return;
    }
    if (length > m_maxRxPayload) {
        // This is what FlowStatus::Overflow is FOR: tell the ECU we cannot
        // take the message rather than half-buffering it and timing out.
        // (A 12-bit length can never exceed MaxPayload, so comparing against
        // that constant instead of the configurable cap would be dead code.)
        sendFlowControl(OverflowAbort);
        emit errorOccurred(PayloadTooLong,
                           tr("The ECU announced %1 bytes; this tester accepts %2.")
                           .arg(length).arg(m_maxRxPayload));
        return;
    }

    m_rxBuffer.clear();
    m_rxBuffer.reserve(length);
    for (int i = 2; i < 8; ++i)
        m_rxBuffer.append(char(frame.payload[i]));

    m_rxExpected = length;
    m_rxSeq = 1;
    m_rxFramesInBlock = 0;
    m_rxState = RxReceiving;

    if (!sendFlowControl(ContinueToSend)) {
        abortRx(TransmitFailed, tr("Could not send Flow Control - is an interface connected?"));
        return;
    }
    m_consecutiveTimer->start(ConsecutiveTimeoutMs);
}

void IsoTpLayer::handleConsecutiveFrame(const CanFrame &frame)
{
    if (m_rxState != RxReceiving)
        return;                                  // nothing is being reassembled

    const quint8 sequence = frame.payload[0] & 0x0F;
    if (sequence != m_rxSeq) {
        abortRx(SequenceNumberMismatch,
                tr("Expected consecutive frame %1 but got %2 - frames were lost.")
                .arg(m_rxSeq).arg(sequence));
        return;
    }
    emit framePassed(false, QStringLiteral("CF"), frameBytes(frame));

    const int missing = m_rxExpected - m_rxBuffer.size();
    const int available = qMin<int>(int(frame.dlc) - 1, 7);
    const int chunk = qMin(missing, available);
    for (int i = 0; i < chunk; ++i)
        m_rxBuffer.append(char(frame.payload[i + 1]));

    m_rxSeq = quint8((m_rxSeq + 1) & 0x0F);
    ++m_rxFramesInBlock;

    if (m_rxBuffer.size() >= m_rxExpected) {
        finishRx();
        return;
    }

    if (m_localBlockSize != 0 && m_rxFramesInBlock >= int(m_localBlockSize)) {
        m_rxFramesInBlock = 0;
        if (!sendFlowControl(ContinueToSend)) {
            abortRx(TransmitFailed, tr("Could not send Flow Control mid-message."));
            return;
        }
    }
    m_consecutiveTimer->start(ConsecutiveTimeoutMs);
}

void IsoTpLayer::handleFlowControl(const CanFrame &frame)
{
    if (m_txState != TxWaitFlowControl)
        return;                                  // not waiting for one
    if (frame.dlc < 3)
        return;

    emit framePassed(false, QStringLiteral("FC"), frameBytes(frame));
    const quint8 status = frame.payload[0] & 0x0F;

    switch (status) {
    case ContinueToSend:
        m_flowControlTimer->stop();
        m_txBlockSize = frame.payload[1];
        m_txStMinMs = decodeStMin(frame.payload[2]);
        m_txFramesInBlock = 0;
        m_txState = TxSending;
        // The first CF of a block goes out immediately; STmin is the gap
        // BETWEEN consecutive frames, not before the first one.
        sendNextConsecutiveFrame();
        break;

    case WaitForNext:
        // The ECU is not ready. Keep waiting, but restart the clock so a
        // stream of Waits cannot hold the link open forever between them.
        m_flowControlTimer->start(FlowControlTimeoutMs);
        break;

    case OverflowAbort:
    default:
        abortTx(PeerOverflow, tr("The ECU cannot accept a message this long."));
        break;
    }
}

bool IsoTpLayer::sendFlowControl(FlowStatus status)
{
    QByteArray fc;
    fc.append(char(0x30 | (status & 0x0F)));
    fc.append(char(m_localBlockSize));
    fc.append(char(m_localStMin));
    const bool ok = writeFrame(fc);
    if (ok)
        emit framePassed(true, QStringLiteral("FC"), fc.toHex(' ').toUpper());
    return ok;
}

void IsoTpLayer::finishRx()
{
    m_consecutiveTimer->stop();
    // Trailing padding beyond the announced length is not part of the message.
    const QByteArray message = m_rxBuffer.left(m_rxExpected);
    m_rxState = RxIdle;
    m_rxBuffer.clear();
    m_rxExpected = 0;
    m_rxFramesInBlock = 0;
    emit messageReceived(message);
}

// -------------------------------------------------------------------- errors

void IsoTpLayer::onFlowControlTimeout()
{
    abortTx(FlowControlTimeout, tr("No Flow Control from the ECU within %1 ms.")
            .arg(FlowControlTimeoutMs));
}

void IsoTpLayer::onConsecutiveTimeout()
{
    abortRx(ConsecutiveFrameTimeout,
            tr("The ECU stopped sending after %1 of %2 bytes.")
            .arg(m_rxBuffer.size()).arg(m_rxExpected));
}

void IsoTpLayer::abortTx(Error error, const QString &text)
{
    m_flowControlTimer->stop();
    m_stMinTimer->stop();
    m_txState = TxIdle;
    m_txPayload.clear();
    m_txOffset = 0;
    m_txFramesInBlock = 0;
    emit errorOccurred(error, text);
}

void IsoTpLayer::abortRx(Error error, const QString &text)
{
    m_consecutiveTimer->stop();
    m_rxState = RxIdle;
    m_rxBuffer.clear();
    m_rxExpected = 0;
    m_rxFramesInBlock = 0;
    emit errorOccurred(error, text);
}

void IsoTpLayer::reset()
{
    m_flowControlTimer->stop();
    m_consecutiveTimer->stop();
    m_stMinTimer->stop();
    m_txState = TxIdle;
    m_txPayload.clear();
    m_txOffset = 0;
    m_txFramesInBlock = 0;
    m_rxState = RxIdle;
    m_rxBuffer.clear();
    m_rxExpected = 0;
    m_rxFramesInBlock = 0;
    QMutexLocker locker(&m_inboxMutex);
    m_inbox.clear();
}

// ------------------------------------------------------------------- helpers

int IsoTpLayer::decodeStMin(quint8 raw)
{
    // 0x00-0x7F is milliseconds outright.
    if (raw <= 0x7F)
        return int(raw);

    // 0xF1-0xF9 is 100..900 MICROseconds. QTimer cannot pace that, and the
    // one direction that is never legal is sending FASTER than the ECU asked
    // for - so a sub-millisecond request is rounded UP to 1 ms rather than
    // down to 0. Slower than requested is always allowed; faster is not.
    if (raw >= 0xF1 && raw <= 0xF9)
        return 1;

    // Everything else is reserved, and ISO 15765-2 is explicit that a
    // reserved value must be treated as the maximum, not ignored.
    return ReservedStMinMs;
}

QString IsoTpLayer::frameBytes(const CanFrame &frame)
{
    QString out;
    out.reserve(frame.dlc * 3);
    for (quint8 i = 0; i < frame.dlc && i < frame.payload.size(); ++i) {
        if (i > 0)
            out += QLatin1Char(' ');
        out += QStringLiteral("%1").arg(frame.payload[i], 2, 16, QLatin1Char('0')).toUpper();
    }
    return out;
}
