#pragma once

#include <QCanBusDevice>
#include <QCanBusFrame>
#include <QObject>
#include <QStringList>
#include <QVector>

#include <functional>

#include "core/CanFrame.h"

/*!
    \brief Thin wrapper around one QCanBusDevice, living on its own thread.

    Why a worker thread: QCanBusDevice delivers frames through its own event
    loop, and at 5000+ frames/s the read-and-convert work would otherwise
    compete with rendering on the GUI thread. Everything here - createDevice(),
    connectDevice(), readAllFrames() - therefore runs on the device thread, and
    the only thing that crosses to the GUI is a batch of already-converted
    CanFrames.

    Thread affinity rules for this class:
      - construct it anywhere, then moveToThread() BEFORE calling open();
      - call open()/close()/send() only through a queued invocation
        (CanBusManager does this);
      - framesReady() is emitted ON THE DEVICE THREAD and is deliberately
        connected with Qt::DirectConnection, because its consumer
        (CanLogModel::enqueue) is thread-safe and does its own coalescing.
        Routing every batch through the GUI event loop would copy the data an
        extra time for no benefit.
*/
class CanDevice : public QObject
{
    Q_OBJECT

public:
    /*!
        \a clock must return the bus-clock timestamp in nanoseconds and be
        callable from this object's thread (CanBusManager passes its
        QElapsedTimer reader).
    */
    explicit CanDevice(std::function<qint64()> clock, QObject *parent = nullptr);
    ~CanDevice() override;

    /*! Plugins Qt actually has available in this build ("socketcan", "peakcan", ...). */
    static QStringList availablePlugins();
    /*! Interface names a plugin currently offers ("can0", "usbcan0", ...). */
    static QStringList availableInterfaces(const QString &plugin);
    /*! socketcan on Linux, peakcan on Windows, virtualcan as the fallback. */
    static QString preferredPlugin();

    /*!
        Cleans a string that came out of a vendor driver.

        Plugins hand back fixed-size C buffers. The PCAN one in particular
        returns its message padded with whatever was left in the buffer, so
        errorString() can carry hundreds of NULs and stray bytes after the
        readable text - which QML then renders as a wall of boxes. Cut at the
        first NUL, drop anything unprintable, collapse whitespace and cap the
        length, so a driver can never blow up the UI with its padding.
    */
    static QString sanitize(const QString &text, int maxLength = 240);

public slots:
    void open(const QString &plugin, const QString &interfaceName, int bitrate);
    void close();
    void send(const CanFrame &frame);
    /*! Writes an already-built QCanBusFrame. Device thread only (queued). */
    void writeRaw(const QCanBusFrame &frame);

signals:
    /*! One readAllFrames() batch, emitted on the device thread. */
    void framesReady(const QVector<CanFrame> &frames);
    /*! QCanBusDevice::CanBusDeviceState, as an int so QML/queued signals stay simple. */
    void stateChanged(int state);
    void errorText(const QString &message);
    /*! Error frames are counted but kept out of the trace. */
    void errorFramesSeen(int count);

private:
    void handleFramesReceived();
    void teardown();

    std::function<qint64()> m_clock;
    QCanBusDevice *m_device = nullptr;

    // Hardware timestamps are relative to the driver's own epoch. The first
    // frame anchors that epoch onto our bus clock so both agree from then on.
    bool m_hardwareEpochValid = false;
    qint64 m_hardwareEpochUs = 0;
    qint64 m_busEpochNs = 0;
};
