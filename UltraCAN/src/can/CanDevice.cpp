#include "CanDevice.h"

#include <QCanBus>
#include <QCanBusDeviceInfo>
#include <QDebug>
#include <QVariant>   // setConfigurationParameter() takes a QVariant
#include <QtGlobal>   // Q_OS_LINUX / Q_OS_WIN

#if defined(Q_OS_LINUX)
#  include <QDir>
#  include <QFile>
#endif

#include <utility>

namespace {

#if defined(Q_OS_LINUX)

/*! ARPHRD_CAN, from <linux/if_arp.h>. The kernel writes it into
    /sys/class/net/<iface>/type for every CAN network device. */
constexpr int kArphrdCan = 280;

/*!
    The kernel's own list of CAN interfaces, read straight from sysfs.

    This is the FALLBACK for socketcan, used only when QCanBus enumerates
    nothing. Qt's socketcan backend walks if_nameindex() and applies the same
    ARPHRD_CAN test, so in the normal case both lists agree and this code never
    runs; it earns its place when the backend's enumeration path is the thing
    that failed (a SerialBus build without libsocketcan, a container where the
    netlink query is refused) and the interface is nevertheless openable.

    vcan and slcan devices report ARPHRD_CAN as well, which is deliberate: a
    vcan0 from `modprobe vcan` must appear exactly like real hardware, because
    for everything above this layer it behaves like it.
*/
QStringList socketCanInterfacesFromSysfs()
{
    const QDir sysClassNet(QStringLiteral("/sys/class/net"));
    QStringList names;

    // The entries are symlinks INTO /sys/devices; QDir::Dirs follows them.
    const QStringList entries =
            sysClassNet.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::System);
    for (const QString &name : entries) {
        QFile type(sysClassNet.filePath(name) + QStringLiteral("/type"));
        if (!type.open(QIODevice::ReadOnly | QIODevice::Text))
            continue;                    // lo, a device disappearing mid-scan
        bool ok = false;
        if (type.readAll().trimmed().toInt(&ok) == kArphrdCan && ok)
            names.append(name);
    }

    names.sort();                        // can0, can1, vcan0 - stable ordering
    return names;
}

#endif // Q_OS_LINUX

} // namespace

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

bool CanDevice::isSocketCan(const QString &plugin)
{
    return plugin.compare(QLatin1String("socketcan"), Qt::CaseInsensitive) == 0;
}

QStringList CanDevice::availablePlugins()
{
    if (!QCanBus::instance())
        return {};

    QStringList plugins = QCanBus::instance()->plugins();

#if defined(Q_OS_LINUX)
    // SocketCAN is the native transport here: can0 and vcan0 are kernel network
    // devices, not vendor hardware behind a vendor DLL, so it is what a Linux
    // user reaches for first and it belongs at the top of the picker. The
    // connection panel selects entry 0 when nothing is remembered and nothing
    // is preferred, so the order is not merely cosmetic.
    //
    // Deliberately a RE-ORDER and not an insert. A name QCanBus does not know
    // is not a plugin this application can open - createDevice() would fail
    // with "cannot find plugin" the moment somebody picked it - so fabricating
    // the entry would trade an honest empty list for a dead menu item that
    // fails at the worst possible moment. Qt's Linux SerialBus build always
    // ships this plugin; its absence means the module was configured without
    // it, which is worth saying out loud exactly once per process.
    const qsizetype socketcan = plugins.indexOf(QLatin1String("socketcan"));
    if (socketcan > 0) {
        plugins.move(socketcan, 0);
    } else if (socketcan < 0) {
        static bool warned = false;      // GUI thread only; see the class docs
        if (!warned) {
            warned = true;
            qWarning("This Qt SerialBus build ships no socketcan plugin, so "
                     "can0/vcan0 cannot be opened. Available plugins: %s",
                     plugins.isEmpty() ? "(none)" : qPrintable(plugins.join(QStringLiteral(", "))));
        }
    }
#endif

    return plugins;
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

#if defined(Q_OS_LINUX)
    // Ask the kernel directly when the backend came back empty-handed. An
    // interface that exists in /sys/class/net can be opened by name even if
    // the plugin's own enumeration failed, and the panel disables Connect
    // while the list is empty - so without this a perfectly usable can0 is
    // simply unreachable from the UI.
    if (names.isEmpty() && isSocketCan(plugin)) {
        names = socketCanInterfacesFromSysfs();
        if (!names.isEmpty()) {
            qInfo("socketcan: the plugin enumerated nothing (%s); using the "
                  "kernel's own list instead: %s",
                  errorMessage.isEmpty() ? "no reason given" : qPrintable(sanitize(errorMessage)),
                  qPrintable(names.join(QStringLiteral(", "))));
        }
    }
#endif

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

    // --- plugin-specific configuration ---------------------------------------
    if (isSocketCan(plugin)) {
        // A SocketCAN interface is configured OUTSIDE this application: the
        // bitrate belongs to the kernel's netlink configuration, set once with
        //     sudo ip link set can0 type can bitrate 500000
        //     sudo ip link set up can0
        // and the socket the plugin opens inherits it.
        //
        // BitRateKey is therefore not merely ignored here - it is REFUSED. Qt's
        // socketcan backend only implements the key when SerialBus was built
        // against libsocketcan; everywhere else it falls through to the
        // backend's default branch, which raises a ConfigurationError. That
        // error reaches the connection panel as a red "no such configuration"
        // line sitting underneath a bus that connected perfectly well. Skip the
        // key and tell the user the command that actually changes the rate.
        if (bitrate > 0) {
            qInfo("socketcan: %s runs at the bitrate the kernel gave it; the "
                  "%d bit/s selected here is not applied. To change it: "
                  "sudo ip link set %s down && sudo ip link set %s type can "
                  "bitrate %d && sudo ip link set %s up",
                  qPrintable(interfaceName), bitrate, qPrintable(interfaceName),
                  qPrintable(interfaceName), bitrate, qPrintable(interfaceName));
        }

        // Error frames reach a SocketCAN socket only if the socket asked for
        // them - the kernel's default error mask is empty. Without this the
        // error-frame counter reads 0 on a bus that is error-passive or
        // bus-off, which is precisely when it is worth reading. Other plugins
        // deliver error frames unconditionally, so this is socketcan-only.
        m_device->setConfigurationParameter(
                QCanBusDevice::ErrorFilterKey,
                QVariant::fromValue(QCanBusFrame::FrameErrors(QCanBusFrame::AnyError)));
    } else if (bitrate > 0) {
        // Not every remaining plugin honours BitRateKey either (virtualcan
        // ignores it), but they accept it silently.
        m_device->setConfigurationParameter(QCanBusDevice::BitRateKey, QVariant(bitrate));
    }

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
        QString message = tr("Cannot connect %1/%2: %3")
                          .arg(plugin, interfaceName, sanitize(m_device->errorString()));

        // The overwhelmingly common SocketCAN failure is an interface that
        // exists but has never been brought up, and the kernel's own wording
        // for it ("Network is down") tells the user nothing about the two
        // commands that fix it. Say them.
        if (isSocketCan(plugin)) {
            message += QLatin1Char(' ');
            message += interfaceName.startsWith(QLatin1String("vcan"))
                       ? tr("Run: sudo modprobe vcan; sudo ip link add dev %1 "
                            "type vcan; sudo ip link set up %1")
                         .arg(interfaceName)
                       : tr("Run: sudo ip link set %1 type can bitrate %2; "
                            "sudo ip link set up %1")
                         .arg(interfaceName).arg(bitrate > 0 ? bitrate : 500000);
        }

        emit errorText(message);
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
