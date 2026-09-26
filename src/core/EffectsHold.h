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

#include <cstddef>
#include <cstdint>
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

    // Hand `data` to `send` with every effect plug's channel at 0 when `held`, then put
    // each channel back -- on a throw too. Put back in reverse, so two plugs on one
    // channel leave it as it came in.
    template <typename Send>
    void apply(uint8_t* data, bool held, Send&& send) {
        if (!held || m_channels.empty()) {
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
    std::vector<unsigned int> m_channels;
    std::vector<uint8_t> m_saved;
};

}  // namespace tplink
