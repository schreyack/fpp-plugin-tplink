// Include drogon framework header before FPP headers to avoid
// macro conflicts between trantor's LOG_* macros and FPP's LogLevel enum
#include <drogon/HttpAppFramework.h>
#undef LOG_WARN
#undef LOG_INFO
#undef LOG_DEBUG

#include <fpp-pch.h>

#include <fstream>
#include <sstream>
#include <string>
#include <algorithm>
#include <cstring>
#include <execution>
#include <istream>
#include <ostream>
#include <iostream>
#include <map>
#include <thread>
#include <vector>

#include <unistd.h>
#include <termios.h>
#include <chrono>
#include <thread>
#include <cmath>

#include "fpphttp.h"
#include "common.h"
#include "settings.h"
#include "Plugin.h"
#include "Plugins.h"
#include "log.h"
#include "MultiSync.h"

#if __has_include("channeloutput/ChannelOutputSetup.h")
#include "channeloutput/ChannelOutputSetup.h"
#elif __has_include("channeloutput/channeloutput.h")
#include "channeloutput/channeloutput.h"
#endif

#include "fppversion_defines.h"

#include "commands/Commands.h"

#include "TPLinkLight.h"
#include "TPLinkSwitch.h"
#include "TPLinkItem.h"

#include "BaseLight.h"
#include "BaseSwitch.h"
#include "BaseItem.h"

#include "GoveeLight.h"
#include "TasmotaLight.h"
#include "TasmotaSwitch.h"
#include "TapoLight.h"
#include "TapoSwitch.h"

#include "core/DeviceNames.h"
#include "core/EffectsHold.h"
#include "core/KasaProtocol.h"
#include "core/RelaySender.h"

using namespace std::chrono_literals;

class TPLinkPlugin : public FPPPlugin {
private:
    std::vector<std::unique_ptr <BaseItem>> _TPLinkOutputs;
    Json::Value config;
    // Commands registered with CommandManager. addCommand() does not take
    // ownership, so these are withdrawn and deleted again in shutdown().
    std::vector<Command*> _commands;
    // Set at shutdown so detached toggle threads give up promptly.
    std::atomic<bool> m_shuttingDown{false};

    // --- Effects hold ---
    // When held, the plugin treats the effect plugs' channels as zero in
    // sendChannelData, preventing the show's effect cues from switching
    // the plugs. The hold defaults to ON (fail safe: effects do not fire)
    // and survives restarts via a state file. The house controller sets it
    // through the /TPLink/effects-hold API or the FPP Command.
    std::atomic<bool> m_effectsHeld{true};

    // The channels that are held (0-based) and the work on a frame. Populated at
    // startup from configured switches whose 1-based start channel is non-zero,
    // however many there are, and from the DMX effect machines the config lists
    // (the "effectchannels" entry): those go out at 0 and are not put back
    // (src/core/EffectsHold.h).
    tplink::EffectsHold m_effectsHold;
    // Set when the config has an "effectchannels" entry, so a config without one is said so.
    bool m_effectChannelsListed = false;

    static std::string effectsHeldPath() {
#if FPP_MAJOR_VERSION < 6
        return "/home/fpp/media/config/fpp-plugin-tplink-effects-held";
#else
        return std::string(FPP_DIR_CONFIG("/fpp-plugin-tplink-effects-held"));
#endif
    }

    void loadEffectsHeld() {
        try {
            std::ifstream f(effectsHeldPath());
            if (f.is_open()) {
                std::string line;
                if (std::getline(f, line)) {
                    bool held = (line != "0");
                    m_effectsHeld.store(held, std::memory_order_relaxed);
                    LogInfo(VB_PLUGIN, "Effects hold loaded: %s\n", held ? "held" : "released");
                    return;
                }
            }
        } catch (...) {}
        // Missing or unreadable: default to held (fail safe).
        m_effectsHeld.store(true, std::memory_order_relaxed);
        LogInfo(VB_PLUGIN, "Effects hold state file missing or unreadable; defaulting to held\n");
    }

    bool saveEffectsHeld(bool held) {
        try {
            std::ofstream f(effectsHeldPath());
            if (f.is_open()) {
                f << (held ? "1" : "0") << "\n";
                f.close();
                return true;
            }
        } catch (...) {}
        LogInfo(VB_PLUGIN, "Could not save effects hold state\n");
        return false;
    }

public:
    TPLinkPlugin() : FPPPlugin("fpp-plugin-tplink") {
        LogInfo(VB_PLUGIN, "Initializing TP-Link Plugin\n");
        readFiles();
        loadEffectsHeld();
        registerCommand();
    }
    virtual ~TPLinkPlugin() {
        stopSequenceControl();
        _TPLinkOutputs.clear();
    }

    // Stops every plug's sequence sender: all are told first, then each is
    // waited for, so one slow plug does not hold up telling the rest.
    void stopSequenceControl() {
        for (auto& output : _TPLinkOutputs) {
            if (auto* sw = dynamic_cast<BaseSwitch*>(output.get())) {
                sw->RequestStopSequenceControl();
            }
        }
        for (auto& output : _TPLinkOutputs) {
            if (auto* sw = dynamic_cast<BaseSwitch*>(output.get())) {
                sw->StopSequenceControl();
            }
        }
    }

    // The configured devices a command names, of one kind (switches or
    // lights). `problem` says, in words, which names matched nothing or were
    // the wrong kind; it is also logged.
    template <typename T>
    struct Found {
        std::vector<T*> found;
        std::string problem;
    };

    template <typename T>
    Found<T> findDevices(std::string const& target, const char* kind) {
        std::vector<std::string> configured;
        configured.reserve(_TPLinkOutputs.size());
        for (auto& output : _TPLinkOutputs) {
            configured.push_back(output->GetName());
        }
        auto resolved = tplink::names::resolve(target, configured);
        Found<T> out;
        std::vector<std::string> wrongKind;
        for (std::size_t idx : resolved.indices) {
            if (auto* device = dynamic_cast<T*>(_TPLinkOutputs[idx].get())) {
                out.found.push_back(device);
            } else {
                wrongKind.push_back(_TPLinkOutputs[idx]->GetName());
            }
        }
        if (tplink::names::trim(target).empty()) {
            out.problem = std::string("no device named; pick a ") + kind;
        }
        for (auto const& name : resolved.unknown) {
            out.problem += (out.problem.empty() ? "" : "; ") + std::string("no device named '") + name + "'";
        }
        for (auto const& name : wrongKind) {
            out.problem += (out.problem.empty() ? "" : "; ") + std::string("'") + name + "' is not a " + kind;
        }
        if (!out.problem.empty()) {
            LogWarn(VB_PLUGIN, "TPLink: %s\n", out.problem.c_str());
        }
        return out;
    }

    Found<BaseSwitch> findSwitches(std::string const& target) { return findDevices<BaseSwitch>(target, "switch"); }
    Found<BaseLight> findLights(std::string const& target) { return findDevices<BaseLight>(target, "light"); }

    // The names of the configured devices of one kind, for the command dropdowns.
    template <typename T>
    std::string namesJson() const {
        Json::Value names(Json::arrayValue);
        for (auto& output : _TPLinkOutputs) {
            if (dynamic_cast<T*>(output.get())) {
                names.append(output->GetName());
            }
        }
        Json::StreamWriterBuilder wb;
        wb["indentation"] = "";
        return Json::writeString(wb, names);
    }

    // --- Effects hold public API ---
    bool isEffectsHeld() const {
        return m_effectsHeld.load(std::memory_order_relaxed);
    }

    void setEffectsHeld(bool held) {
        m_effectsHeld.store(held, std::memory_order_relaxed);
        saveEffectsHeld(held);
        LogInfo(VB_PLUGIN, "Effects hold set to: %s\n", held ? "held" : "released");
    }

    // Every command names its device the way plugin.tplink.json does
    // ("LED Roof"), never by IP. The box is a dropdown of the configured
    // switches or lights; a comma list of names also works.
    static constexpr const char* kSwitchesUrl = "api/plugin-apis/TPLink/switches";
    static constexpr const char* kLightsUrl = "api/plugin-apis/TPLink/lights";

    static Command::CommandArg deviceArg(const char* url) {
        return Command::CommandArg("device", "string", "Device").setContentListUrl(url);
    }

    static std::unique_ptr<Command::Result> resultOf(std::string const& done, std::string const& problem) {
        if (problem.empty()) {
            return std::make_unique<Command::Result>(done);
        }
        return std::make_unique<Command::ErrorResult>(problem);
    }

    class TPLinkSetSwitchCommand : public Command {
    public:
        TPLinkSetSwitchCommand(TPLinkPlugin *p) : Command("TPLink Set Switch"), plugin(p) {
            args.push_back(deviceArg(kSwitchesUrl));
            args.push_back(CommandArg("state", "bool", "Set Switch On or Off").setDefaultValue("true"));
        }

        virtual std::unique_ptr<Command::Result> run(const std::vector<std::string> &args) override {
            std::string const device = args.size() >= 1 ? args[0] : "";
            bool const on = args.size() < 2 || args[1] == "true";
            auto switches = plugin->findSwitches(device);
            for (auto* sw : switches.found) {
                sw->EnableOutput();
                sw->requestState(on);
            }
            return resultOf("TPLink Switch Set", switches.problem);
        }
        TPLinkPlugin *plugin;
    };

    class TPLinkToggleSwitchCommand : public Command {
    public:
        TPLinkToggleSwitchCommand(TPLinkPlugin *p) : Command("TPLink Toggle Switch"), plugin(p) {
            args.push_back(deviceArg(kSwitchesUrl));
            args.push_back(CommandArg("delay", "int", "Delay MS").setRange(1, 10000).setDefaultValue("100"));
        }

        virtual std::unique_ptr<Command::Result> run(const std::vector<std::string> &args) override {
            std::string const device = args.size() >= 1 ? args[0] : "";
            std::chrono::milliseconds delay = 10ms;
            if (args.size() >= 2) {
                delay = std::chrono::milliseconds(std::stoi(args[1]));
            }
            auto switches = plugin->findSwitches(device);
            // Run the off-sleep-on cycle on a detached thread so the
            // command returns to the caller immediately.
            std::thread([p = plugin, found = switches.found, delay]() {
                if (p->m_shuttingDown.load()) return;
                for (auto* sw : found) {
                    sw->EnableOutput();
                    sw->requestState(false);
                }
                auto deadline = std::chrono::steady_clock::now() + delay;
                while (std::chrono::steady_clock::now() < deadline) {
                    if (p->m_shuttingDown.load()) return;
                    std::this_thread::sleep_for(50ms);
                }
                if (p->m_shuttingDown.load()) return;
                for (auto* sw : found) {
                    sw->requestState(true);
                }
            }).detach();
            return resultOf("TPLink Switch Toggle", switches.problem);
        }
        TPLinkPlugin *plugin;
    };

    class TPLinkSetLightRGBCommand : public Command {
    public:
        TPLinkSetLightRGBCommand(TPLinkPlugin *p) : Command("TPLink Set Light RGB"), plugin(p) {
            args.push_back(deviceArg(kLightsUrl));
            args.push_back(CommandArg("r", "int", "Red").setRange(0, 255).setDefaultValue("255"));
            args.push_back(CommandArg("g", "int", "Green").setRange(0, 255).setDefaultValue("255"));
            args.push_back(CommandArg("b", "int", "Blue").setRange(0, 255).setDefaultValue("255"));
            args.push_back(CommandArg("color_temp", "int", "Color Temp").setRange(0, 9000).setDefaultValue("0"));
            args.push_back(CommandArg("period", "int", "Delay in ms").setRange(0, 30000).setDefaultValue("0"));
        }

        virtual std::unique_ptr<Command::Result> run(const std::vector<std::string> &args) override {
            std::string const device = args.size() >= 1 ? args[0] : "";
            uint8_t r = 255;
            uint8_t g = 255;
            uint8_t b = 255;
            int colorTemp = 0;
            int period = 0;
            if (args.size() >= 2) {
                r = std::stoi(args[1]);
            }
            if (args.size() >= 3) {
                g = std::stoi(args[2]);
            }
            if (args.size() >= 4) {
                b = std::stoi(args[3]);
            }
            if (args.size() >= 5) {
                colorTemp = std::stoi(args[4]);
            }
            if (args.size() >= 6) {
                period = std::stoi(args[5]);
            }
            auto lights = plugin->findLights(device);
            for (auto* light : lights.found) {
                light->EnableOutput();
                light->setLightOnRGB(r, g, b, colorTemp, period);
            }
            return resultOf("TPLink Light RGB Set", lights.problem);
        }
        TPLinkPlugin *plugin;
    };

    class TPLinkSetLightHSVCommand : public Command {
    public:
        TPLinkSetLightHSVCommand(TPLinkPlugin *p) : Command("TPLink Set Light HSV"), plugin(p) {
            args.push_back(deviceArg(kLightsUrl));
            args.push_back(CommandArg("hue", "int", "Hue").setRange(0, 360).setDefaultValue("1"));
            args.push_back(CommandArg("sat", "int", "Saturation").setRange(0, 100).setDefaultValue("100"));
            args.push_back(CommandArg("bright", "int", "Brightness").setRange(0, 100).setDefaultValue("100"));
            args.push_back(CommandArg("color_temp", "int", "Color Temp").setRange(0, 9000).setDefaultValue("0"));
            args.push_back(CommandArg("period", "int", "Delay in ms").setRange(0, 30000).setDefaultValue("0"));
        }

        virtual std::unique_ptr<Command::Result> run(const std::vector<std::string> &args) override {
            std::string const device = args.size() >= 1 ? args[0] : "";
            int hue = 1;
            int sat = 100;
            int bright = 100;
            int colorTemp = 0;
            int period = 0;
            if (args.size() >= 2) {
                hue = std::stoi(args[1]);
            }
            if (args.size() >= 3) {
                sat = std::stoi(args[2]);
            }
            if (args.size() >= 4) {
                bright = std::stoi(args[3]);
            }
            if (args.size() >= 5) {
                colorTemp = std::stoi(args[4]);
            }
            if (args.size() >= 6) {
                period = std::stoi(args[5]);
            }
            auto lights = plugin->findLights(device);
            for (auto* light : lights.found) {
                light->EnableOutput();
                light->setLightOnHSV(hue, sat, bright, colorTemp, period);
            }
            return resultOf("TPLink Light HSV Set", lights.problem);
        }
        TPLinkPlugin *plugin;
    };

    class TPLinkSetLightOffCommand : public Command {
    public:
        TPLinkSetLightOffCommand(TPLinkPlugin *p) : Command("TPLink Set Light Off"), plugin(p) {
            args.push_back(deviceArg(kLightsUrl));
        }

        virtual std::unique_ptr<Command::Result> run(const std::vector<std::string> &args) override {
            std::string const device = args.size() >= 1 ? args[0] : "";
            auto lights = plugin->findLights(device);
            for (auto* light : lights.found) {
                light->EnableOutput();
                light->setLightOff();
            }
            return resultOf("TPLink Light Off Set", lights.problem);
        }
        TPLinkPlugin *plugin;
    };

    class TPLinkAllSwitchesOffCommand : public Command {
    public:
        TPLinkAllSwitchesOffCommand(TPLinkPlugin *p) : Command("TPLink All Switches Off"), plugin(p) {
        }

        virtual std::unique_ptr<Command::Result> run(const std::vector<std::string> &args) override {

            plugin->turnSwitchesOff();
            return std::make_unique<Command::Result>("TPLink All Switches Off");
        }
        TPLinkPlugin *plugin;
    };

    class TPLinkAllSwitchesOnCommand : public Command {
    public:
        TPLinkAllSwitchesOnCommand(TPLinkPlugin *p) : Command("TPLink All Switches On"), plugin(p) {
        }

        virtual std::unique_ptr<Command::Result> run(const std::vector<std::string> &args) override {

            plugin->turnSwitchesOn();
            return std::make_unique<Command::Result>("TPLink All Switches On");
        }
        TPLinkPlugin *plugin;
    };

    class TPLinkAllLightsRGBCommand : public Command {
    public:
        TPLinkAllLightsRGBCommand(TPLinkPlugin *p) : Command("TPLink All Lights RGB"), plugin(p) {
            args.push_back(CommandArg("r", "int", "Red").setRange(0, 255).setDefaultValue("255"));
            args.push_back(CommandArg("g", "int", "Green").setRange(0, 255).setDefaultValue("255"));
            args.push_back(CommandArg("b", "int", "Blue").setRange(0, 255).setDefaultValue("255"));
            args.push_back(CommandArg("color_temp", "int", "Color Temp").setRange(0, 9000).setDefaultValue("0"));
            args.push_back(CommandArg("period", "int", "Delay in ms").setRange(0, 30000).setDefaultValue("0"));
        }

        virtual std::unique_ptr<Command::Result> run(const std::vector<std::string> &args) override {
            uint8_t r = 255;
            uint8_t g = 255;
            uint8_t b = 255;
            int colorTemp = 0;
            int period = 0;
            if (args.size() >= 1) {
                r = std::stoi(args[0]);
            }
            if (args.size() >= 2) {
                g = std::stoi(args[1]);
            }
            if (args.size() >= 3) {
                b = std::stoi(args[2]);
            }
            if (args.size() >= 4) {
                colorTemp = std::stoi(args[3]);
            }
            if (args.size() >= 5) {
                period = std::stoi(args[4]);
            }
            plugin->turnLightsRGB( r, g, b, colorTemp, period);
            return std::make_unique<Command::Result>("TPLink All Lights RGB Set");
        }
        TPLinkPlugin *plugin;
    };

    class TPLinkAllLightsHSVCommand : public Command {
    public:
        TPLinkAllLightsHSVCommand(TPLinkPlugin *p) : Command("TPLink All Lights HSV"), plugin(p) {
            args.push_back(CommandArg("hue", "int", "Hue").setRange(0, 360).setDefaultValue("1"));
            args.push_back(CommandArg("sat", "int", "Saturation").setRange(0, 100).setDefaultValue("100"));
            args.push_back(CommandArg("bright", "int", "Brightness").setRange(0, 100).setDefaultValue("100"));
            args.push_back(CommandArg("color_temp", "int", "Color Temp").setRange(0, 9000).setDefaultValue("0"));
            args.push_back(CommandArg("period", "int", "Delay in ms").setRange(0, 30000).setDefaultValue("0"));
        }

        virtual std::unique_ptr<Command::Result> run(const std::vector<std::string> &args) override {
            int hue = 1;
            int sat = 100;
            int bright = 100;
            int colorTemp = 0;
            int period = 0;
            if (args.size() >= 1) {
                hue = std::stoi(args[0]);
            }
            if (args.size() >= 2) {
                sat = std::stoi(args[1]);
            }
            if (args.size() >= 3) {
                bright = std::stoi(args[2]);
            }
            if (args.size() >= 4) {
                colorTemp = std::stoi(args[3]);
            }
            if (args.size() >= 5) {
                period = std::stoi(args[4]);
            }
            plugin->turnLightsHSV( hue, sat, bright, colorTemp, period);
            return std::make_unique<Command::Result>("TPLink All Lights HSV Set");
        }
        TPLinkPlugin *plugin;
    };

    class TPLinkAllLightsOffCommand : public Command {
    public:
        TPLinkAllLightsOffCommand(TPLinkPlugin *p) : Command("TPLink All Lights Off"), plugin(p) {
        }

        virtual std::unique_ptr<Command::Result> run(const std::vector<std::string> &args) override {

            plugin->turnLightsOff();
            return std::make_unique<Command::Result>("TPLink All Lights Off");
        }
        TPLinkPlugin *plugin;
    };

    class TPLinkAllSwitchesToggleCommand : public Command {
    public:
        TPLinkAllSwitchesToggleCommand(TPLinkPlugin *p) : Command("TPLink All Switches Toggle"), plugin(p) {
            args.push_back(CommandArg("delay", "int", "Delay MS").setRange(1, 10000).setDefaultValue("100"));
        }

        virtual std::unique_ptr<Command::Result> run(const std::vector<std::string> &args) override {
            std::chrono::milliseconds delay = 10ms;
            if (args.size() >= 1) {
                delay = std::chrono::milliseconds(std::stoi(args[0]));
            }
            std::thread([p = plugin, delay]() {
                if (p->m_shuttingDown.load()) return;
                p->turnSwitchesOff();
                auto deadline = std::chrono::steady_clock::now() + delay;
                while (std::chrono::steady_clock::now() < deadline) {
                    if (p->m_shuttingDown.load()) return;
                    std::this_thread::sleep_for(50ms);
                }
                if (p->m_shuttingDown.load()) return;
                p->turnSwitchesOn();
            }).detach();
            return std::make_unique<Command::Result>("TPLink All Switches Toggle");
        }
        TPLinkPlugin *plugin;
    };

    // FPP Command: hold or release the effect plugs.
    class TPLinkEffectsHoldCommand : public Command {
    public:
        TPLinkEffectsHoldCommand(TPLinkPlugin *p) : Command("TPLink Effects Hold"), plugin(p) {
            args.push_back(CommandArg("held", "bool", "Hold effects off").setDefaultValue("true"));
        }
        virtual std::unique_ptr<Command::Result> run(const std::vector<std::string> &args) override {
            bool held = true;
            if (args.size() >= 1) {
                held = (args[0] == "true" || args[0] == "1");
            }
            plugin->setEffectsHeld(held);
            return std::make_unique<Command::Result>(held ? "Effects held" : "Effects released");
        }
        TPLinkPlugin *plugin;
    };

    void registerCommand() {
        _commands.push_back(new TPLinkSetSwitchCommand(this));
        _commands.push_back(new TPLinkToggleSwitchCommand(this));
        _commands.push_back(new TPLinkSetLightRGBCommand(this));
        _commands.push_back(new TPLinkSetLightHSVCommand(this));
        _commands.push_back(new TPLinkSetLightOffCommand(this));
        _commands.push_back(new TPLinkAllSwitchesOnCommand(this));
        _commands.push_back(new TPLinkAllSwitchesOffCommand(this));
        _commands.push_back(new TPLinkAllLightsRGBCommand(this));
        _commands.push_back(new TPLinkAllLightsHSVCommand(this));
        _commands.push_back(new TPLinkAllLightsOffCommand(this));
        _commands.push_back(new TPLinkAllSwitchesToggleCommand(this));
        _commands.push_back(new TPLinkEffectsHoldCommand(this));
        for (Command* c : _commands) {
            CommandManager::INSTANCE.addCommand(c);
        }
    }

    // Called by FPP once the plugin's HTTP routes are disarmed and before the
    // plugin object is destroyed (plugin API 6). removeCommand() only
    // unregisters, so the commands are ours to delete here; anything left
    // behind would be a call into this plugin after it is gone.
    //
    // The toggle commands sleep for up to 10s inside run() before calling
    // back into this object, so ask FPP to hold off destroying the object
    // long enough for those to drain.
    virtual std::function<bool()> shutdown() override {
        m_shuttingDown.store(true);
        stopSequenceControl();
        for (Command* c : _commands) {
            CommandManager::INSTANCE.removeCommand(c);
            delete c;
        }
        _commands.clear();
        long long until = GetTimeMS() + 12000;
        return [until]() { return GetTimeMS() >= until; };
    }

    void handleTopicsRequest(const HttpRequestPtr &req,
                              std::function<void(const HttpResponsePtr &)> &&callback) {
        callback(makeStringResponse(getTopics(), 200));
    }

    void handleEffectsHoldRequest(const HttpRequestPtr &req,
                                   std::function<void(const HttpResponsePtr &)> &&callback) {
        if (req->getMethod() == drogon::Get) {
            Json::Value resp;
            resp["held"] = isEffectsHeld();
            Json::StreamWriterBuilder wb;
            wb["indentation"] = "";
            callback(makeStringResponse(Json::writeString(wb, resp), 200, "application/json"));
        } else if (req->getMethod() == drogon::Put || req->getMethod() == drogon::Post) {
            Json::Value body;
            Json::CharReaderBuilder rb;
            std::string errs;
            auto bodyView = req->getBody();
            std::string bodyStr(bodyView.data(), bodyView.size());
            std::istringstream ss(bodyStr);
            if (Json::parseFromStream(rb, ss, &body, &errs) && body.isMember("held")) {
                bool held = body["held"].asBool();
                setEffectsHeld(held);
                Json::Value resp;
                resp["held"] = held;
                Json::StreamWriterBuilder wb;
                wb["indentation"] = "";
                callback(makeStringResponse(Json::writeString(wb, resp), 200, "application/json"));
            } else {
                callback(makeStringResponse("{\"error\":\"expected JSON with a 'held' field\"}", 400, "application/json"));
            }
        } else {
            callback(makeStringResponse("{\"error\":\"use GET or PUT\"}", 405, "application/json"));
        }
    }

    void unregisterApis() override {
        FPPPlugins::unregisterPluginApi("/TPLink");
        FPPPlugins::unregisterPluginApi("/TPLink/effects-hold");
        FPPPlugins::unregisterPluginApi("/TPLink/switches");
        FPPPlugins::unregisterPluginApi("/TPLink/lights");
    }
    void registerApis() override {
        FPPPlugins::registerPluginApi(
            "/TPLink",
            [this](const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback) {
                handleTopicsRequest(req, std::move(callback));
            },
            { drogon::Get });
        FPPPlugins::registerPluginApi(
            "/TPLink/effects-hold",
            [this](const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback) {
                handleEffectsHoldRequest(req, std::move(callback));
            },
            { drogon::Get, drogon::Put, drogon::Post });
        // The names behind the commands' device dropdowns (a JSON array).
        FPPPlugins::registerPluginApi(
            "/TPLink/switches",
            [this](const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback) {
                callback(makeStringResponse(namesJson<BaseSwitch>(), 200, "application/json"));
            },
            { drogon::Get });
        FPPPlugins::registerPluginApi(
            "/TPLink/lights",
            [this](const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback) {
                callback(makeStringResponse(namesJson<BaseLight>(), 200, "application/json"));
            },
            { drogon::Get });
    }

    // Plugs follow the channel data FPP is about to SEND, after pixel overlays
    // (modifyChannelData), not the raw sequence (modifySequenceData, which runs
    // before overlays). While the effects hold is on, the plugin zeroes the
    // effect plugs' channels before passing them to the switches, so the show's
    // cues cannot fire those plugs. This replaces the old overlay-based gate,
    // which kept FPP's channel output thread alive and caused dark frames to
    // interleave with DMXLive's DDP output = blink.
    virtual void modifyChannelData(int ms, uint8_t *seqData) override {
        try
        {
            sendChannelData(seqData);
        }
        catch(std::exception const& ex)
        {
            std::cout << ex.what();
        }
    }

    virtual void playlistCallback(const Json::Value &playlist, const std::string &action, const std::string &section, int item) {
        if (settings["Start"] == "PlaylistStart" && action == "start") {
            EnableTPLinkItems();
        }
    }

    void EnableTPLinkItems() {
        for(auto & output: _TPLinkOutputs) {
            output->EnableOutput();
        }
    }

    // Runs on fppd's output thread for every frame. Each output's SendData is a
    // compare, plus a hand-off or a thread start when its value changes, so a
    // plain loop is all this needs.
    //
    // When effects are held, the effect plugs' channels are temporarily zeroed
    // in the data buffer so every switch sees 0 and stays off. The buffer is
    // restored afterwards so downstream plugins are unaffected. Every effect plug
    // is held, however many there are (src/core/EffectsHold.h).
    void sendChannelData(unsigned char *data) {
        m_effectsHold.apply(data, m_effectsHeld.load(std::memory_order_relaxed), [this](uint8_t* frame) {
            for (auto& output : _TPLinkOutputs) {
                output->SendData(frame);
            }
        });
    }

    void turnSwitchesOff() {
        for (auto& output : _TPLinkOutputs) {
            auto* derived = dynamic_cast<BaseSwitch*>(output.get());
            if (derived) {
                derived->EnableOutput();
                derived->requestState(false);
            }
        }
    }

    void turnSwitchesOn() {
        for (auto& output : _TPLinkOutputs) {
            auto* derived = dynamic_cast<BaseSwitch*>(output.get());
            if (derived) {
                derived->EnableOutput();
                derived->requestState(true);
            }
        }
    }

    void turnLightsRGB(uint8_t r, uint8_t g, uint8_t b, int color_temp, int period) {
        std::for_each(std::execution::par, std::begin(_TPLinkOutputs), std::end(_TPLinkOutputs),
         [r, g, b, color_temp, period](auto& output) {
            auto* derived = dynamic_cast<BaseLight*>(output.get());
            if (derived) {
                derived->EnableOutput();
                derived->setLightOnRGB(r, g, b, color_temp, period);
            }
        });
    }

    void turnLightsHSV(int hue, int sat, int bright, int color_temp, int period) {
        std::for_each(std::execution::par, std::begin(_TPLinkOutputs), std::end(_TPLinkOutputs),
         [hue, sat, bright, color_temp, period](auto& output) {
            auto* derived = dynamic_cast<BaseLight*>(output.get());
            if (derived) {
                derived->EnableOutput();
                derived->setLightOnHSV(hue, sat, bright, color_temp, period);
            }
        });
    }

    void turnLightsOff() {
        std::for_each(std::execution::par, std::begin(_TPLinkOutputs), std::end(_TPLinkOutputs), [](auto& output) {
            auto* derived = dynamic_cast<BaseLight*>(output.get());
            if (derived) {
                derived->EnableOutput();
                derived->setLightOff();
            }
        });
    }

    void saveDataToFile()
    {
        std::ofstream outfile;
#if FPP_MAJOR_VERSION < 6
        outfile.open ("/home/fpp/media/config/fpp-plugin-tplink");
#else
        outfile.open (FPP_DIR_CONFIG("/fpp-plugin-tplink"));
#endif

        if(_TPLinkOutputs.size() ==0) {
            outfile <<  "nooutputsfound;1;null";
            outfile <<  "\n";
        }

        for(auto & out: _TPLinkOutputs) {
            outfile << out->GetIPAddress();
            outfile <<  ";";
            outfile << out->GetStartChannel();
            outfile <<  ";";
            outfile << out->GetType();
            outfile <<  ";";
            outfile << out->GetName();
            outfile <<  "\n";
        }
        outfile.close();
    }

    // The "effectchannels" entry of plugin.tplink.json, written by Studio from the rig's fog
    // fixtures: {"effectchannels": [{"name": "...", "first": N, "count": M}, ...]}, `first` the
    // 1-based FPP channel. While the hold is on these channels go out at 0 (EffectsHold). A range
    // that is not whole is named in the log and left out; the others still hold.
    void readEffectChannels(Json::Value const& entry)
    {
        m_effectChannelsListed = true;
        Json::Value const& list = entry["effectchannels"];
        if (!list.isArray()) {
            LogErr(VB_PLUGIN, "TPLink: the effectchannels entry has no list, so no DMX effect machine is held\n");
            return;
        }
        for (Json::Value const& range : list) {
            if (!range.isObject() || !range["first"].isUInt64() || !range["count"].isUInt64()) {
                LogErr(VB_PLUGIN, "TPLink: an effectchannels range is not held: it needs a whole-number first and count\n");
                continue;
            }
            std::string const name = range.get("name", "").asString();
            unsigned long const first = static_cast<unsigned long>(range["first"].asUInt64());
            unsigned long const count = static_cast<unsigned long>(range["count"].asUInt64());
            std::string why;
            if (m_effectsHold.addZeroedRange(first, count, FPPD_MAX_CHANNELS, why)) {
                LogInfo(VB_PLUGIN, "Effect channels held at 0 while effects are held: '%s' %lu-%lu\n",
                        name.c_str(), first, first + count - 1);
            } else {
                LogErr(VB_PLUGIN, "TPLink: effect channels '%s' (%lu, %lu) not held: %s\n",
                       name.c_str(), first, count, why.c_str());
            }
        }
    }

    void readFiles()
    {
        //read topic, payload and start channel settings from JSON setting file.
#if FPP_MAJOR_VERSION < 6
        std::string configLocation = ("/home/fpp/media/config/plugin.tplink.json");
#else
        std::string configLocation = FPP_DIR_CONFIG("/plugin.tplink.json");
#endif
        if (LoadJsonFromFile(configLocation, config)) {
            for (unsigned int i = 0; i < config.size(); i++) {
                std::string const ip = config[i]["ip"].asString();
                std::string const devicetype = config[i].get("devicetype","light").asString();
                if (devicetype == "effectchannels") {
                    // Not a device: Studio's list of the DMX effect machines (fog, smoke).
                    readEffectChannels(config[i]);
                    continue;
                }
                unsigned int sc =  config[i].get("startchannel",1).asInt();
                if(!ip.empty()) {
                    std::unique_ptr<BaseItem> tplinkItem;
                    std::string const username = config[i].get("username", "").asString();
                    std::string const password = config[i].get("password", "").asString();
                    if (devicetype.find("tapolight") != std::string::npos) {
                        tplinkItem = std::make_unique<TapoLight>(ip, sc, username, password);
                    } else if (devicetype.find("taposwitch") != std::string::npos) {
                        int const plugNum =  config[i].get("plugnumber", 0).asInt();
                        tplinkItem = std::make_unique<TapoSwitch>(ip, sc, plugNum, username, password);
                    } else if (devicetype.find("goveelight") != std::string::npos) {
                        tplinkItem = std::make_unique<GoveeLight>(ip, sc);
                    } else if (devicetype.find("tasmotalight") != std::string::npos) {
                        tplinkItem = std::make_unique<TasmotaLight>(ip, sc);
                    } else if (devicetype.find("tasmotaswitch") != std::string::npos) {
                        int const plugNum =  config[i].get("plugnumber", 0).asInt();
                        tplinkItem = std::make_unique<TasmotaSwitch>(ip, sc, plugNum);
                    } else if (devicetype.find("light") != std::string::npos) {
                        tplinkItem = std::make_unique<TPLinkLight>(ip, sc);
                    } else if (devicetype.find("switch") != std::string::npos) {
                        int const plugNum =  config[i].get("plugnumber", 0).asInt();
                        tplinkItem = std::make_unique<TPLinkSwitch>(ip, sc, plugNum);
                    } else {
                        LogInfo(VB_PLUGIN, "Devicetype not found '%s'", devicetype.c_str());
                        tplinkItem = std::make_unique<TPLinkLight>(ip, sc);
                    }
                    std::string name = tplink::names::trim(config[i].get("name", "").asString());
                    if (name.empty()) {
                        LogWarn(VB_PLUGIN, "TPLink: device at %s has no name; commands cannot reach it until it has one\n", ip.c_str());
                    }
                    tplinkItem->SetName(name);
                    LogInfo(VB_PLUGIN, "Added %s\n", tplinkItem->GetConfigString().c_str());
                    if (auto* sw = dynamic_cast<BaseSwitch*>(tplinkItem.get())) {
                        // A plug with a start channel follows it; see BaseSwitch::SendData.
                        sw->StartSequenceControl();
                    }
                    _TPLinkOutputs.push_back(std::move(tplinkItem));
                }
            }
        }
        saveDataToFile();

        std::vector<std::string> names;
        for (auto& output : _TPLinkOutputs) {
            names.push_back(output->GetName());
        }
        for (auto const& name : tplink::names::duplicates(names)) {
            if (!name.empty()) {
                LogWarn(VB_PLUGIN, "TPLink: more than one device is named '%s'; commands reach only the first\n", name.c_str());
            }
        }

        // Build the list of effect plug channel indices (0-based). A switch
        // with a non-zero start channel that falls in the effect plug range
        // is held when the effects hold is active.
        std::vector<unsigned int> effectPlugChannels;
        for (auto& output : _TPLinkOutputs) {
            unsigned int sc = output->GetStartChannel();
            if (sc != 0 && dynamic_cast<BaseSwitch*>(output.get())) {
                effectPlugChannels.push_back(sc - 1);  // 0-based index
            }
        }
        m_effectsHold.setChannels(std::move(effectPlugChannels));
        if (!m_effectChannelsListed) {
            LogWarn(VB_PLUGIN, "TPLink: plugin.tplink.json has no effectchannels entry, so the effects hold keeps the effect plugs off but does not zero any DMX effect machine\n");
        }
        if (!m_effectsHold.channels().empty()) {
            LogInfo(VB_PLUGIN, "Effect plug channels (1-based):");
            for (unsigned int idx : m_effectsHold.channels()) {
                LogInfo(VB_PLUGIN, "  %u\n", idx + 1);
            }
        }
    }

    std::string getTopics()
    {
        std::string topics;
        for(auto & out: _TPLinkOutputs) {
            topics += out->GetName();
            topics += ",";
        }
        return topics;
    }

};


extern "C" {
    FPPPlugin *createPlugin() {
        return new TPLinkPlugin();
    }
}
