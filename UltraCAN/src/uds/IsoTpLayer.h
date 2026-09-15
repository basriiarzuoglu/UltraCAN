#pragma once

#include "core/CanFrame.h"

#include <QByteArray>
#include <QMutex>
#include <QObject>
#include <QVector>
#include <QtQml/qqmlregistration.h>

class CanBusManager;
class QTimer;

/*!
    \brief ISO 15765-2 (ISO-TP) transport layer over classic CAN.

    Turns the 8-byte frame the bus can carry into the arbitrary-length message
    a diagnostic protocol needs, in both directions:

    \list
    \li \b Tx - a payload of 1..7 bytes goes out as a Single Frame; anything
        longer becomes a First Frame plus a run of Consecutive Frames, paced
        by whatever the ECU asks for in its Flow Control.
    \li \b Rx - a First Frame opens a reassembly buffer, this layer answers
        with Flow Control, and the Consecutive Frames are stitched back into
        one \l messageReceived().
    \endlist

    \section2 Threading

    \l ingest() is called ON THE CAN DEVICE THREAD from
    CanBusManager::fanOut(), exactly like the analyzer and graph sinks. It
    does the one cheap thing that must happen there - drop every frame that
    is not addressed to us - and hands the survivors to the owning thread
    through a queued call. That matters: this class runs a state machine with
    timers, and QTimer belongs to the thread that owns the object. On a busy
    bus the common case is "no frame matched", which costs a mutex lock and a
    comparison per batch and posts nothing.

    \section2 What is deliberately not implemented

    \list
    \li \b {Payloads over 4095 bytes}. The 2016 edition adds an escape
        sequence (FF with a zero length nibble, then a 32-bit length) for
        those. Nothing in a normal UDS exchange needs it, and a half-tested
        path here would be worse than an explicit refusal - see \l MaxPayload.
    \li \b {Extended / mixed addressing}, where the first payload byte is an
        address extension. This layer speaks NORMAL addressing (11-bit or
        29-bit CAN ids, PCI in byte 0), which is what OBD-II and virtually
        every UDS-on-CAN ECU uses.
    \li \b {CAN FD}. SF/FF encodings differ above 8 data bytes.
    \endlist
*/
class IsoTpLayer : public QObject
{
    Q_OBJECT
    QML_ANONYMOUS

public:
    /*! Flow-control status, the low nibble of a FC frame's byte 0. */
    enum FlowStatus : quint8 {
        ContinueToSend = 0x0,
        WaitForNext    = 0x1,
        OverflowAbort  = 0x2
    };
    Q_ENUM(FlowStatus)

    enum Error {
        NoError = 0,
        FlowControlTimeout,        //!< N_Bs - the ECU never answered our FF
        ConsecutiveFrameTimeout,   //!< N_Cr - the ECU stopped mid-message
        SequenceNumberMismatch,    //!< a CF arrived out of order: frames were lost
        PayloadTooLong,            //!< over MaxPayload, or over our Rx buffer cap
        PeerOverflow,              //!< the ECU answered FC with Overflow
        TransmitFailed,            //!< the bus refused the write
        Busy                       //!< a send is already in flight
    };
    Q_ENUM(Error)

    /*! 12 bits of length in a First Frame. See the class note. */
    static constexpr int MaxPayload = 4095;
    /*! N_Bs: how long we wait for Flow Control after a FF or a full block. */
    static constexpr int FlowControlTimeoutMs = 1000;
    /*! N_Cr: how long we wait for the next Consecutive Frame. */
    static constexpr int ConsecutiveTimeoutMs = 1000;
    /*! ISO 15765-2 says a reserved STmin value is to be treated as 127 ms. */
    static constexpr int ReservedStMinMs = 127;

    explicit IsoTpLayer(QObject *parent = nullptr);
    ~IsoTpLayer() override;

    /*! Where frames are written. Wired once in main(). */
    void setBus(CanBusManager *bus);

    // -- addressing -----------------------------------------------------------
    quint32 txId() const { return m_txId; }
    quint32 rxId() const { return m_rxId; }
    /*! Both ids at once: they are always changed together, and a half-applied
        pair would have us listening to the old ECU while talking to the new. */
    void setAddressing(quint32 txId, quint32 rxId, bool extendedIds);
    bool extendedIds() const { return m_extendedIds; }

    // -- what WE advertise in the Flow Control we send ------------------------
    /*! Frames the ECU may send before waiting for our next FC. 0 = no limit. */
    void setLocalBlockSize(quint8 blockSize) { m_localBlockSize = blockSize; }
    /*! Minimum gap we ask the ECU to leave between its CFs, in milliseconds. */
    void setLocalStMin(quint8 stMinMs) { m_localStMin = stMinMs; }

    /*! Pad every frame to 8 bytes. On by default: many ECUs ignore short
        frames outright, and the CAN-level cost of the padding is zero. */
    void setPadding(bool enabled, quint8 value = 0xCC);

    /*!
        Largest reassembly this layer will accept, in bytes.

        Defaults to \l MaxPayload, i.e. accept anything the protocol can
        announce. Lower it and a First Frame asking for more is refused with
        a Flow Control Overflow instead of being buffered - which is the
        whole reason FlowStatus has an Overflow code. Worth setting on a
        memory-constrained target, or to prove an ECU handles the refusal.
    */
    void setMaxRxPayload(int bytes);
    int maxRxPayload() const { return m_maxRxPayload; }

    /*! True while a segmented send is in flight or a message is reassembling. */
    bool busy() const;

    /*! Segments and sends \a payload. False means it was rejected outright -
        \l errorOccurred() carries why - not that the ECU failed to answer. */
    bool send(const QByteArray &payload);

    /*! Abandons both directions and clears the timers. */
    void reset();

    /*!
        Frame ingress, ON THE DEVICE THREAD. See the threading note.
    */
    void ingest(const QVector<CanFrame> &frames);

signals:
    /*! One fully reassembled message from the ECU. */
    void messageReceived(const QByteArray &data);
    /*! The last frame of an outgoing message has been written to the bus. */
    void messageSent(const QByteArray &data);
    void errorOccurred(int error, const QString &text);
    /*! Every ISO-TP frame in or out, for the diagnostics console. \a kind is
        one of "SF", "FF", "CF", "FC". */
    void framePassed(bool outgoing, const QString &kind, const QString &bytes);

protected:
    /*!
        The single seam between this state machine and the bus.

        Protected and virtual on purpose: a loopback test overrides it to feed
        two layers into each other, which is the only way to exercise the
        FF/FC/CF handshake without a bus and an ECU on the other end of it.
    */
    virtual bool writeFrame(const QByteArray &pciAndData);

private slots:
    /*! Runs on the OWNING thread; drains what ingest() filtered. */
    void drainInbox();
    void onFlowControlTimeout();
    void onConsecutiveTimeout();
    void sendNextConsecutiveFrame();

private:
    enum TxState { TxIdle, TxWaitFlowControl, TxSending };
    enum RxState { RxIdle, RxReceiving };

    void handleFrame(const CanFrame &frame);
    void handleSingleFrame(const CanFrame &frame);
    void handleFirstFrame(const CanFrame &frame);
    void handleConsecutiveFrame(const CanFrame &frame);
    void handleFlowControl(const CanFrame &frame);

    bool sendFlowControl(FlowStatus status);
    void finishRx();
    void abortTx(Error error, const QString &text);
    void abortRx(Error error, const QString &text);
    static int decodeStMin(quint8 raw);
    static QString frameBytes(const CanFrame &frame);

    CanBusManager *m_bus = nullptr;

    quint32 m_txId = 0x7E0;
    quint32 m_rxId = 0x7E8;
    bool m_extendedIds = false;
    bool m_padding = true;
    quint8 m_paddingByte = 0xCC;

    int m_maxRxPayload = MaxPayload;
    quint8 m_localBlockSize = 0;
    quint8 m_localStMin = 0;

    // -- tx half
    TxState m_txState = TxIdle;
    QByteArray m_txPayload;
    int m_txOffset = 0;
    quint8 m_txSeq = 1;
    quint8 m_txBlockSize = 0;      //!< what the ECU asked for
    int m_txStMinMs = 0;           //!< what the ECU asked for, already decoded
    int m_txFramesInBlock = 0;

    // -- rx half
    RxState m_rxState = RxIdle;
    QByteArray m_rxBuffer;
    int m_rxExpected = 0;
    quint8 m_rxSeq = 1;
    int m_rxFramesInBlock = 0;

    QTimer *m_flowControlTimer = nullptr;
    QTimer *m_consecutiveTimer = nullptr;
    QTimer *m_stMinTimer = nullptr;

    QMutex m_inboxMutex;
    QVector<CanFrame> m_inbox;
    bool m_drainPending = false;
};
