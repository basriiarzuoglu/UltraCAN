#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QtQml/qqmlregistration.h>

#include "IsoTpLayer.h"

class CanBusManager;
class QTimer;

/*!
    \brief ISO 14229 (UDS) client on top of \l IsoTpLayer.

    One request at a time, which is what the protocol allows: UDS on CAN has
    no transaction id, so a second request in flight makes the next response
    ambiguous. \l busy tells the UI when to grey its buttons out.

    \section2 Response pending is not an error

    An ECU that needs longer than P2 to answer replies 0x7F <sid> 0x78
    (requestCorrectlyReceived-ResponsePending) and then keeps replying it
    until the real answer is ready. Treating that as a negative response -
    the obvious reading of "0x7F" - makes Clear DTCs and Read DTCs look like
    they fail on most real ECUs, because those are exactly the services slow
    enough to send it. Here 0x78 restarts the clock at \l pendingTimeoutMs
    and keeps waiting.
*/
class UdsClient : public QObject
{
    Q_OBJECT
    QML_ANONYMOUS

    Q_PROPERTY(quint32 txId READ txId WRITE setTxId NOTIFY addressingChanged FINAL)
    Q_PROPERTY(quint32 rxId READ rxId WRITE setRxId NOTIFY addressingChanged FINAL)
    Q_PROPERTY(bool extendedIds READ extendedIds WRITE setExtendedIds NOTIFY addressingChanged FINAL)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged FINAL)
    Q_PROPERTY(bool busReady READ busReady NOTIFY busReadyChanged FINAL)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged FINAL)
    /*! The transport underneath, so the diagnostics console can subscribe to
        IsoTpLayer::framePassed and show the raw SF/FF/CF/FC traffic. CONSTANT:
        the client owns one transport for its whole life. */
    Q_PROPERTY(IsoTpLayer *transport READ transport CONSTANT FINAL)

public:
    /*! The services this client knows how to build a request for. */
    enum Service : quint8 {
        DiagnosticSessionControl = 0x10,
        EcuReset                 = 0x11,
        ClearDiagnosticInfo      = 0x14,
        ReadDtcInformation       = 0x19,
        ReadDataByIdentifier     = 0x22,
        TesterPresent            = 0x3E
    };
    Q_ENUM(Service)

    explicit UdsClient(QObject *parent = nullptr);
    ~UdsClient() override;

    void setBus(CanBusManager *bus);
    /*! The transport. Exposed so main() can hand the same instance to
        CanBusManager as a frame sink. */
    IsoTpLayer *transport() const { return m_tp; }

    quint32 txId() const { return m_txId; }
    quint32 rxId() const { return m_rxId; }
    bool extendedIds() const { return m_extendedIds; }
    void setTxId(quint32 id);
    void setRxId(quint32 id);
    void setExtendedIds(bool extended);

    bool busy() const { return m_pendingService != 0; }
    bool busReady() const;
    QString lastError() const { return m_lastError; }

    // -- services -------------------------------------------------------------
    /*! 0x19 0x02 reportDTCByStatusMask. \a statusMask 0xFF asks for every DTC
        the ECU holds regardless of status bits. */
    Q_INVOKABLE bool requestReadDtc(int statusMask = 0xFF);
    /*! 0x14 clearDiagnosticInformation. 0xFFFFFF is "all groups". */
    Q_INVOKABLE bool clearDtc(quint32 groupOfDtc = 0xFFFFFF);
    /*! 0x22 readDataByIdentifier - the live-parameter read. */
    Q_INVOKABLE bool readDataByIdentifier(int identifier);
    /*! 0x10 - session control. 0x01 default, 0x03 extended. */
    Q_INVOKABLE bool diagnosticSessionControl(int session);
    /*! 0x3E 0x00 - keeps a non-default session alive. */
    Q_INVOKABLE bool testerPresent();

    /*! Raw request from the console: "22 01 00", "2201 00" and "220100" all
        parse. Returns false and sets \l lastError on a malformed string. */
    Q_INVOKABLE bool sendHexRequest(const QString &hexText);
    bool sendRequest(const QByteArray &payload);

    /*! Gives up on the outstanding request. */
    Q_INVOKABLE void abort();

    // -- decoding, exposed for the UI and for tests ---------------------------
    /*! "P0128-00" from the three DTC bytes of a 0x19 record. */
    Q_INVOKABLE static QString formatDtc(quint8 high, quint8 middle, quint8 low);
    /*! Human-readable list of the status bits set in a 0x19 record. */
    Q_INVOKABLE static QString describeDtcStatus(quint8 status);
    /*! The standard negative-response-code table. */
    Q_INVOKABLE static QString describeNrc(quint8 nrc);
    /*! Bytes as "22 01 00". */
    Q_INVOKABLE static QString toHex(const QByteArray &data);

signals:
    void addressingChanged();
    void busyChanged();
    void busReadyChanged();
    void lastErrorChanged();

    /*! Any positive response, whole, including the service id byte. */
    void responseReceived(const QByteArray &data);
    /*! Decoded 0x19 result: one entry per DTC, "P0128-00  confirmed". */
    void dtcRead(const QStringList &dtcs);
    /*! 0x7F. \a nrc already excludes 0x78, which is handled internally. */
    void negativeResponse(int service, int nrc, const QString &text);
    /*! The ECU asked for more time. Worth showing: the UI would otherwise
        look frozen for seconds during a Clear DTCs. */
    void responsePending(int service);
    /*! What went out, for the console. */
    void requestSent(const QByteArray &data);
    void errorOccurred(const QString &text);

private slots:
    void onMessageReceived(const QByteArray &data);
    void onTransportError(int error, const QString &text);
    void onTimeout();

private:
    void setLastError(const QString &text);
    void finish();
    QStringList decodeDtcResponse(const QByteArray &data) const;

    /*! P2_server. One second is generous for a normal service and short
        enough that a dead ECU does not hang the button. */
    static constexpr int DefaultTimeoutMs = 1000;
    /*! P2*_server, used once the ECU has said 0x78. */
    static constexpr int PendingTimeoutMs = 5000;
    static constexpr quint8 NegativeResponse = 0x7F;
    static constexpr quint8 ResponsePending = 0x78;

    CanBusManager *m_bus = nullptr;
    IsoTpLayer *m_tp = nullptr;
    QTimer *m_timer = nullptr;

    quint32 m_txId = 0x7E0;
    quint32 m_rxId = 0x7E8;
    bool m_extendedIds = false;

    quint8 m_pendingService = 0;      //!< 0 when idle
    QString m_lastError;
};
