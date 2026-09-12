#include "BaseSwitch.h"

//#include "common.h"
#include "log.h"

#include <exception>

BaseSwitch::BaseSwitch(std::string const& ip, unsigned int startChannel, int plug_num) :
BaseItem(ip,startChannel),
m_plug_num(plug_num)
{
}

BaseSwitch::~BaseSwitch() {
    StopSequenceControl();
}

// A plug with a start channel follows that channel: 127 or more is on, below
// 127 is off. It is switched only when that state changes, and until the
// channel has been seen on, off does nothing. The rule is tplink::RelayFollower;
// the sending is the plug's tplink::RelaySender.
bool BaseSwitch::SendData( unsigned char *data) {
    if (m_startChannel == 0 || !m_sequence) {
        return false;
    }
    try {
        m_sequence->onChannelValue(data[m_startChannel - 1]);
    } catch (std::exception const& ex) {
        LogInfo(VB_PLUGIN, "Error %s \n", ex.what());
        return false;
    }
    return true;
}

void BaseSwitch::StartSequenceControl() {
    if (m_startChannel == 0 || m_sequence) {
        return;
    }
    try {
        auto sequence = std::make_unique<tplink::SequenceRelay>(
            [this](bool on, std::atomic<bool> const& stop) { return sendForSequence(on, stop); });
        sequence->start();
        m_sequence = std::move(sequence);
    } catch (std::exception const& ex) {
        LogInfo(VB_PLUGIN, "Could not start sequence control for %s: %s\n", m_ipAddress.c_str(), ex.what());
    }
}

void BaseSwitch::RequestStopSequenceControl() {
    if (m_sequence) {
        m_sequence->requestStop();
    }
}

void BaseSwitch::StopSequenceControl() {
    if (m_sequence) {
        m_sequence->stop();
    }
}

bool BaseSwitch::sendRelayState(bool on, std::atomic<bool> const&) {
    return on ? setRelayOn() : setRelayOff();
}

bool BaseSwitch::sendForSequence(bool on, std::atomic<bool> const& stop) {
    const bool ok = sendRelayState(on, stop);
    if (ok) {
        if (m_failedTries > 0) {
            LogInfo(VB_PLUGIN, "Switched %s %s after %u failed tries\n", m_ipAddress.c_str(), on ? "on" : "off",
                    m_failedTries);
        }
        m_failedTries = 0;
    } else if (!stop.load()) {
        if (m_failedTries == 0) {
            LogInfo(VB_PLUGIN, "Could not switch %s %s; retrying until it answers\n", m_ipAddress.c_str(),
                    on ? "on" : "off");
        }
        ++m_failedTries;
    }
    return ok;
}
