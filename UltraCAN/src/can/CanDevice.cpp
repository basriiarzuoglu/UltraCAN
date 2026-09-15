#include "CanDevice.h"

#include <QCanBus>
#include <QCanBusDeviceInfo>
#include <QDebug>
#include <QVariant>   // setConfigurationParameter() takes a QVariant

#include <utility>

CanDevice::CanDevice(std::function<qint64()> clock, QObject *parent)
    : QObject(parent)
    , m_clock(std::move(clock))
{
}

CanDevice::~CanDevice()
{
    teardown();
}

// ------------------------------------------------------------- enumeration

QStringList CanDevice::availablePlugins()
{
    if (!QCanBus::instance())
        return {};
    return QCanBus::instance()->plugins();
}

QStringList CanDevice::availableInterfaces(const QString &plugin)
{
    if (!QCanBus::instance() || plugin.isEmpty())
        return {};

    QString errorMessage;
    const QList<QCanBusDeviceInfo> infos =
            QCanBus::instance()->availableDevices(plugin, &errorMessage);

    QStringList names;
    names.reserve(infos.size());
    for (const QCanBusDeviceInfo &info : infos)
        names.append(info.name());

    // Several plugins (virtualcan among them) enumerate nothing but still
    // accept the conventional interface names.
    if (names.isEmpty() && plugin == QLatin1String("virtualcan"))
        names << QStringLiteral("can0") << QStringLiteral("can1");

    return names;
}

QString CanDevice::sanitize(const QString &text, int maxLength)
{
    // Everything after the first NUL is buffer padding, not message.
    const int nul = text.indexOf(QChar(u'\0'));
    QString cleaned = (nul >= 0) ? text.left(nul) : text;

    QString printable;
    printable.reserve(cleaned.size());
    for (const QChar ch : std::as_const(cleaned)) {
        if (ch.isPrint() || ch == QLatin1Char(' '))
            printable += ch;
        else if (ch == QLatin1Char('\n') || ch == QLatin1Char('\t'))
            printable += QLatin1Char(' ');
        // anything else (NUL, control bytes, unassigned code points) is dropped
    }

    printable = printable.simplified();
    if (printable.size() > maxLength)
        printable = printable.left(maxLength - 1) + QChar(u'\u2026');
    return printable;
}

QString CanDevice::preferredPlugin()
{
    const QStringList plugins = availablePlugins();
    const QStringList wishlist {
#if defined(Q_OS_LINUX)
        QStringLiteral("socketcan"),
#elif defined(Q_OS_WIN)
        QStringLiteral("peakcan"),
        QStringLiteral("vectorcan"),
        QStringLiteral("systeccan"),
        QStringLiteral("ixxatcan"),
#endif
        QStringLiteral("virtualcan")
    };

    for (const QString &candidate : wishlist)
        if (plugins.contains(candidate))
            return candidate;

    return plugins.value(0);
}

// ------------------------------------------------------------- lifecycle

void CanDevice::open(const QString &plugin, const QString &interfaceName, int bitrate)
{
    close();

    if (!QCanBus::instance()) {
        emit errorText(tr("Qt SerialBus is not available in this build."));
        return;
    }

    QString errorMessage;
    m_device = QCanBus::instance()->createDevice(plugin, interfaceName, &errorMessage);
    if (!m_device) {
        emit errorText(tr("Cannot create %1/%2: %3")
                       .arg(plugin, interfaceName, sanitize(errorMessage)));
        emit stateChanged(int(QCanBusDevice::UnconnectedState));
        return;
    }

    // Not every plugin honours BitRateKey (socketcan takes it from `ip link`,
    // virtualcan ignores it). Setting it is harmless where unsupported.
    if (bitrate > 0)
        m_device->setConfigurationParameter(QCanBusDevice::BitRateKey, QVariant(bitrate));

    connect(m_device, &QCanBusDevice::framesReceived,
            this, &CanDevice::handleFramesReceived);

    connect(m_device, &QCanBusDevice::stateChanged, this,
            [this](QCanBusDevice::CanBusDeviceState state) {
                emit stateChanged(int(state));
            });

    connect(m_device, &QCanBusDevice::errorOccurred, this,
            [this](QCanBusDevice::CanBusError) {
                if (m_device)
                    emit errorText(sanitize(m_device->errorString()));
            });

    m_hardwareEpochValid = false;

    if (!m_device->connectDevice()) {
        emit errorText(tr("Cannot connect %1/%2: %3")
                       .arg(plugin, interfaceName, sanitize(m_device->errorString())));
        teardown();
        emit stateChanged(int(QCanBusDevice::UnconnectedState));
    }
}

void CanDevice::close()
{
    if (!m_device)
        return;
    teardown();
    emit stateChanged(int(QCanBusDevice::UnconnectedState));
}

void CanDevice::teardown()
{
    if (!m_device)
        return;

    m_device->disconnect(this);          // stop callbacks before we tear down
    if (m_device->state() != QCanBusDevice::UnconnectedState)
        m_device->disconnectDevice();
    delete m_device;
    m_device = nullptr;
}

// ---------------------------------------------------------------- receive

void CanDevice::handleFramesReceived()
{
    if (!m_device)
        return;

    // readAllFrames() drains the driver queue in one call - this IS the batch.
    const QList<QCanBusFrame> incoming = m_device->readAllFrames();
    if (incoming.isEmpty())
        return;

    QVector<CanFrame> batch;
    batch.reserve(incoming.size());
    int errorFrames = 0;

    for (const QCanBusFrame &source : incoming) {
        if (source.frameType() == QCanBusFrame::ErrorFrame) {
            ++errorFrames;                       // counted, kept out of the trace
            continue;
        }
        if (!source.isValid())
            continue;

        CanFrame frame;
        frame.id = source.frameId();
        frame.extended = source.hasExtendedFrameFormat();
        frame.direction = CanFrame::Rx;
        frame.channel = 0;                       // one device per channel for now

        const QByteArray payload = source.payload();
        frame.dlc = quint8(qMin(payload.size(), qsizetype(frame.payload.size())));
        for (quint8 i = 0; i < frame.dlc; ++i)
            frame.payload[i] = quint8(payload.at(i));

        // Prefer the driver's hardware timestamp, anchored to our bus clock on
        // the first frame, so jitter measurements stay meaningful. Plugins that
        // report nothing fall back to the bus clock.
        const qint64 hardwareUs = source.timeStamp().seconds() * 1'000'000
                                  + source.timeStamp().microSeconds();
        if (hardwareUs > 0) {
            if (!m_hardwareEpochValid) {
                m_hardwareEpochUs = hardwareUs;
                m_busEpochNs = m_clock ? m_clock() : 0;
                m_hardwareEpochValid = true;
            }
            frame.timestampNs = m_busEpochNs + (hardwareUs - m_hardwareEpochUs) * 1000;
        } else {
            frame.timestampNs = m_clock ? m_clock() : 0;
        }

        batch.append(frame);
    }

    if (errorFrames > 0)
        emit errorFramesSeen(errorFrames);
    if (!batch.isEmpty())
        emit framesReady(batch);
}

// --------------------------------------------------------------- transmit

void CanDevice::writeRaw(const QCanBusFrame &frame)
{
    if (!m_device || m_device->state() != QCanBusDevice::ConnectedState)
        return;

    // A malformed frame is refused by the driver without a word - the counter
    // climbs and the bus stays quiet. Say so instead.
    if (!frame.isValid()) {
        emit errorText(tr("Refusing malformed frame: id 0x%1, %2 format, %3 bytes")
                       .arg(frame.frameId(), 0, 16)
                       .arg(frame.hasExtendedFrameFormat() ? tr("extended") : tr("standard"))
                       .arg(frame.payload().size()));
        return;
    }

    if (!m_device->writeFrame(frame))
        emit errorText(tr("Write failed: %1").arg(sanitize(m_device->errorString())));
}

void CanDevice::send(const CanFrame &frame)
{
    if (!m_device || m_device->state() != QCanBusDevice::ConnectedState)
        return;

    QCanBusFrame out;
    // Set the format BEFORE the id: an id above 0x7FF on a standard-format
    // frame makes QCanBusFrame invalid, and the driver then discards it in
    // silence.
    out.setExtendedFrameFormat(frame.extended || frame.id > 0x7FFu);
    out.setFrameId(frame.id);
    out.setFrameType(QCanBusFrame::DataFrame);
    out.setPayload(QByteArray(reinterpret_cast<const char *>(frame.payload.data()),
                              qMin<int>(frame.dlc, int(frame.payload.size()))));

    if (!out.isValid()) {
        emit errorText(tr("Refusing malformed frame: id 0x%1, %2 format, %3 bytes")
                       .arg(frame.id, 0, 16)
                       .arg(out.hasExtendedFrameFormat() ? tr("extended") : tr("standard"))
                       .arg(int(frame.dlc)));
        return;
    }

    if (!m_device->writeFrame(out))
        emit errorText(tr("Write failed: %1").arg(sanitize(m_device->errorString())));
}
