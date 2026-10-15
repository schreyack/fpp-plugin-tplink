#pragma once

#include <stdlib.h>
#include <list>
#include <string>
#include <atomic>

//#include "common.h"
//#include "log.h"

class BaseItem {
public:
    BaseItem(std::string const& ip, unsigned int startChannel );
    virtual ~BaseItem();

    std::string GetIPAddress() const { return m_ipAddress; }
    // The name commands and playlists use for this device ("LED Roof"), from
    // the "name" field of plugin.tplink.json.
    std::string GetName() const { return m_name; }
    void SetName(std::string const& name) { m_name = name; }
    // How the log names this device: "LED Roof (192.168.110.158)".
    std::string GetLabel() const { return m_name.empty() ? m_ipAddress : m_name + " (" + m_ipAddress + ")"; }
    unsigned int GetStartChannel() const { return m_startChannel; }

    virtual void EnableOutput() { m_unreachable = false; m_issending = false; }

    virtual bool SendData(unsigned char *data) = 0;

    virtual std::string GetType() const = 0;// {return "";};
    virtual std::string GetConfigString() const;

protected:
    std::string m_name;
    std::string m_ipAddress;
    
    unsigned int m_startChannel;
    unsigned int m_seqCount;

    std::atomic<bool> m_unreachable;
    std::atomic<bool> m_issending;

};