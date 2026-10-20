#pragma once

// The effects hold's work on a frame, apart from FPP so the host tests can drive it.
//
// While the hold is on, every effect plug's channel reads 0 to the switches, and the
// frame is put back exactly as it came in before any plugin after this one sees it.
// The effect plugs are every switch with a start channel, however many there are:
// the values are saved into a buffer sized when the channels are set, never a fixed
// count, so a ninth plug is held like the first (it used to be a uint8_t[8], and a
// ninth plug's cue went through while effects were held). Setting the channels
// allocates; a frame never does.
//
// The DMX effect machines (fog, smoke) are held the other way: their channels go out
// at 0 and are NOT put back. They are not read by a switch; the frame IS what goes to
// the controllers, so the zero has to stay in it (ticket #1032, Tim: "if the effects
// toggle is off in HA then effects should never fire"). The list of them comes from
// plugin.tplink.json (the entry with devicetype "effectchannels"), written by Studio
// from the rig's fog fixtures.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace tplink {

class EffectsHold {
public:
    // The 0-based channel of every effect plug, in any order, repeats allowed. Set when
    // the plugin reads its switches, before frames flow.
    void setChannels(std::vector<unsigned int> channels) {
        m_channels = std::move(channels);
        m_saved.assign(m_channels.size(), 0);
    }

    std::vector<unsigned int> const& channels() const { return m_channels; }

    // A DMX effect machine's channels, 1-based as FPP numbers them: `count` channels from
    // `first`. They must lie inside a frame of `frameChannels`. True when added; false, with
    // `why` in words, when the range is not whole: nothing is added, so the caller says so
    // loudly and never half-holds a machine. Set when the plugin reads its config, before
    // frames flow; allocates, a frame never does.
    bool addZeroedRange(unsigned long first, unsigned long count, std::size_t frameChannels, std::string& why) {
        if (first < 1 || count < 1) {
            why = "its first channel and its count must both be at least 1";
            return false;
        }
        if (first - 1 + count > frameChannels) {
            why = "it runs past the last channel FPP has (" + std::to_string(frameChannels) + ")";
            return false;
        }
        m_zeroed.push_back({static_cast<std::size_t>(first - 1), static_cast<std::size_t>(count)});
        return true;
    }

    std::size_t zeroedRangeCount() const { return m_zeroed.size(); }

    // When `held`: every DMX effect channel is written 0 in `data` and STAYS 0, so the
    // controllers get 0 (done first, so a plug channel inside one reads 0 too and is put
    // back as 0). Then `data` goes to `send` with every effect plug's channel at 0, and
    // each plug channel is put back -- on a throw too. Put back in reverse, so two plugs
    // on one channel leave it as it came in. Not held: `data` goes to `send` untouched.
    template <typename Send>
    void apply(uint8_t* data, bool held, Send&& send) {
        if (!held) {
            send(data);
            return;
        }
        for (Range const& range : m_zeroed) {
            std::memset(data + range.first, 0, range.count);
        }
        if (m_channels.empty()) {
            send(data);
            return;
        }
        for (std::size_t i = 0; i < m_channels.size(); ++i) {
            m_saved[i] = data[m_channels[i]];
            data[m_channels[i]] = 0;
        }
        struct PutBack {
            EffectsHold& hold;
            uint8_t* data;
            ~PutBack() {
                for (std::size_t i = hold.m_channels.size(); i-- > 0;) {
                    data[hold.m_channels[i]] = hold.m_saved[i];
                }
            }
        } const putBack{*this, data};
        send(data);
    }

private:
    struct Range {
        std::size_t first;  // 0-based
        std::size_t count;
    };

    std::vector<unsigned int> m_channels;
    std::vector<uint8_t> m_saved;
    std::vector<Range> m_zeroed;
};

}  // namespace tplink
