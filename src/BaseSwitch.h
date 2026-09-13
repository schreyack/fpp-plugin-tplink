#pragma once


#include "BaseItem.h"

#include "core/SequenceRelay.h"

#include <atomic>
#include <chrono>
#include <memory>

class BaseSwitch : virtual public BaseItem {
public:
    BaseSwitch(std::string const& ip, unsigned int startChannel , int plug_num );
    virtual ~BaseSwitch();

    // Called for every sequence frame. When the plug's channel crosses half,
    // hands the new ON/OFF state to the plug's sender; otherwise does nothing.
    // Never does network I/O and never allocates.
    bool SendData( unsigned char *data) override;

    void EnableOutput() override;

    // Starts this plug's sender. The plugin calls it once for each configured
    // switch. A start channel of 0 means the plug is commands-only: it still
    // gets a sender thread (so commands are non-blocking) but SendData does
    // nothing.
    void StartSequenceControl();
    // Tells the sender to stop without waiting for it.
    void RequestStopSequenceControl();
    // Stops the sender and waits for its thread. Safe to call more than once.
    // Every concrete switch calls this first in its destructor, because the
    // sender thread calls into the derived object.
    void StopSequenceControl();

    // Hands a command-requested state to this plug's sender. Returns
    // immediately; the sender delivers it on its own thread with retry.
    // Does nothing if the sender has not been started.
    void requestState(bool on);

    virtual bool setRelayOn() = 0;
    virtual bool setRelayOff() = 0;

    virtual bool setLedOn() = 0;
    virtual bool setLedOff() = 0;

    int plugNumber() const { return m_plug_num; }

protected:
    int m_plug_num;

    // How the sender switches the relay for the sequence; runs only on the
    // sender thread. The default calls setRelayOn() or setRelayOff(). An
    // override can give up early once `stop` turns true.
    virtual bool sendRelayState(bool on, std::atomic<bool> const& stop);

private:
    bool sendForSequence(bool on, std::atomic<bool> const& stop);
    void gaveUpForSequence(bool on, unsigned tries, std::chrono::milliseconds window);

    std::unique_ptr<tplink::SequenceRelay> m_sequence;
    unsigned m_failedTries = 0;  // sender thread only
};
