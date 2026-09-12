#pragma once

#include <cstdint>

namespace tplink {

// Decides when a plug's sequence channel switches the plug.
//
// A channel value of 127 or more means ON; below 127 means OFF. The plug is
// switched only when that ON/OFF state changes, and never again while it holds.
//
// The plugin cannot know a plug's real state until the sequence has driven it,
// so until the channel has been seen ON, an OFF value does nothing. A plug
// powered by a command, a playlist lead-in or Home Assistant is never switched
// off just because a sequence leaves its channel at 0. The first ON value is a
// change and switches the plug on; after that a drop below half switches it
// off, and a rise switches it on again.
//
// What this remembers lives as long as the object. A new sequence does not
// reset it.
class RelayFollower {
public:
    enum class Action : uint8_t { None, SwitchOn, SwitchOff };

    static constexpr uint8_t kOnThreshold = 127;

    Action onValue(uint8_t value) noexcept {
        const State now = value >= kOnThreshold ? State::On : State::Off;
        if (now == m_state || (m_state == State::Unknown && now == State::Off)) {
            return Action::None;
        }
        m_state = now;
        return now == State::On ? Action::SwitchOn : Action::SwitchOff;
    }

    // True once the channel has been seen ON.
    bool seenOn() const noexcept { return m_state != State::Unknown; }

private:
    enum class State : uint8_t { Unknown, On, Off };
    State m_state = State::Unknown;
};

}  // namespace tplink
