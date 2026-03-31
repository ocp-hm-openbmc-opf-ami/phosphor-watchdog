#include "watchdog.hpp"

#include <systemd/sd-journal.h>

#include <PDKHooks.hpp>
#include <phosphor-logging/elog.hpp>
#include <phosphor-logging/log.hpp>
#include <sdbusplus/exception.hpp>
#include <xyz/openbmc_project/Common/error.hpp>
#include <xyz/openbmc_project/State/Host/server.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <string>
#include <string_view>

namespace phosphor
{
namespace watchdog
{
using namespace std::chrono;
using namespace std::chrono_literals;
using namespace phosphor::logging;

using sdbusplus::xyz::openbmc_project::Common::Error::InternalFailure;

const static constexpr char* currentHostState = "CurrentHostState";
const static constexpr char* hostStatusOff =
    "xyz.openbmc_project.State.Host.HostState.Off";

const static constexpr char* actionDescription = " due to Watchdog timeout";
const static constexpr char* hardResetDescription = "Hard Reset - System reset";
const static constexpr char* powerOffDescription =
    "Power Down - System power down";
const static constexpr char* powerCycleDescription =
    "Power Cycle - System power cycle";
const static constexpr char* timerExpiredDescription = "Timer expired";

const static constexpr char* preInterruptActionNone =
    "xyz.openbmc_project.State.Watchdog.PreTimeoutInterruptAction.None";

const static constexpr char* preInterruptDescriptionSMI = "SMI";
const static constexpr char* preInterruptDescriptionNMI = "NMI";
const static constexpr char* preInterruptDescriptionMI = "Messaging Interrupt";

const static constexpr char* reservedDescription = "Reserved";

const static constexpr char* timerUseDescriptionBIOSFRB2 = "BIOS FRB2";
const static constexpr char* timerUseDescriptionBIOSPOST = "BIOS/POST";
const static constexpr char* timerUseDescriptionOSLoad = "OSLoad";
const static constexpr char* timerUseDescriptionSMSOS = "SMS/OS";
const static constexpr char* timerUseDescriptionOEM = "OEM";

namespace restart
{
static constexpr const char* busNameBase =
    "xyz.openbmc_project.Control.Host.RestartCause";
static constexpr const char* pathPrefix =
    "/xyz/openbmc_project/control/host";
static constexpr const char* pathSuffix = "/restart_cause";
static constexpr const char* interface =
    "xyz.openbmc_project.Control.Host.RestartCause";
static constexpr const char* property = "RequestedRestartCause";
} // namespace restart

// chassis state manager service
namespace chassis
{
static constexpr const char* busNameBase = "xyz.openbmc_project.State.Chassis";
static constexpr const char* pathPrefix = "/xyz/openbmc_project/state/chassis";
static constexpr const char* pathSuffix = "";
static constexpr const char* interface = "xyz.openbmc_project.State.Chassis";
static constexpr const char* request = "RequestedPowerTransition";
} // namespace chassis

namespace host
{
static constexpr const char* busNameBase = "xyz.openbmc_project.State.Host";
static constexpr const char* pathPrefix = "/xyz/openbmc_project/state/host";
static constexpr const char* pathSuffix = "";
static constexpr const char* interface = "xyz.openbmc_project.State.Host";
static constexpr const char* request = "RequestedHostTransition";
} // namespace host

namespace nmi
{
static constexpr const char* busNameBase =
    "xyz.openbmc_project.Control.Host.NMI";
static constexpr const char* pathPrefix = "/xyz/openbmc_project/control/host";
static constexpr const char* pathSuffix = "/nmi";
static constexpr const char* interface = "xyz.openbmc_project.Control.Host.NMI";
static constexpr const char* request = "NMI";

} // namespace nmi

namespace
{

unsigned int parseHostInstance(std::string_view hostToken)
{
    if (hostToken.empty())
    {
        return 0;
    }

    std::string token(hostToken);
    std::transform(token.begin(), token.end(), token.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });

    static constexpr std::string_view hostPrefix = "host";
    if (token.rfind(hostPrefix, 0) != 0)
    {
        return 0;
    }

    std::string_view suffix(token.data() + hostPrefix.size(),
                            token.size() - hostPrefix.size());
    if (suffix.empty())
    {
        return 0;
    }

    if (!std::all_of(suffix.begin(), suffix.end(),
                     [](unsigned char c) { return std::isdigit(c) != 0; }))
    {
        return 0;
    }

    return static_cast<unsigned int>(std::stoul(std::string(suffix)));
}

unsigned int getInstanceFromObjectPath(std::string_view objectPath)
{
    const auto lastSlash = objectPath.find_last_of('/');
    const std::string_view token =
        (lastSlash == std::string_view::npos)
            ? objectPath
            : objectPath.substr(lastSlash + 1);
    return parseHostInstance(token);
}

std::string buildBusName(std::string_view busNameBase, unsigned int instance)
{
    if (instance == 0)
    {
        return std::string(busNameBase);
    }

    return std::string(busNameBase) + std::to_string(instance);
}

std::string buildPath(std::string_view pathPrefix, unsigned int instance,
                      std::string_view pathSuffix)
{
    return std::string(pathPrefix) + std::to_string(instance) +
           std::string(pathSuffix);
}

} // namespace

void Watchdog::powerStateChangedHandler(
    const std::map<std::string, std::variant<std::string>>& props)
{
    const auto iter = props.find(currentHostState);
    if (iter != props.end())
    {
        const std::string* powerState = std::get_if<std::string>(&iter->second);
        if (powerState && (*powerState == hostStatusOff))
        {
            if (timerEnabled())
            {
                enabled(false);
            }
        }
    }
}

void Watchdog::resetTimeRemaining(bool enableWatchdog)
{
    timeRemaining(interval());
    if (enableWatchdog)
    {
        enabled(true);
    }
}

// Enable or disable watchdog
bool Watchdog::enabled(bool value)
{
    if (!value)
    {
        // Make sure we accurately reflect our enabled state to the
        // tryFallbackOrDisable() call
        WatchdogInherits::enabled(value);

        // Attempt to fallback or disable our timer if needed
        tryFallbackOrDisable();

        return false;
    }
    else if (!this->enabled())
    {
        auto interval_ms = this->interval();
        timer.restart(milliseconds(interval_ms));
        log<level::INFO>("watchdog: enabled and started",
                         entry("INTERVAL=%llu", interval_ms));
    }

    return WatchdogInherits::enabled(value);
}

// Get the remaining time before timer expires.
// If the timer is disabled, returns 0
uint64_t Watchdog::timeRemaining() const
{
    // timer may have already expired and disabled
    if (!timerEnabled())
    {
        return 0;
    }

    return duration_cast<milliseconds>(timer.getRemaining()).count();
}

// Reset the timer to a new expiration value
uint64_t Watchdog::timeRemaining(uint64_t value)
{
    if (!timerEnabled())
    {
        // We don't need to update the timer because it is off
        return 0;
    }

    if (this->enabled())
    {
        // Update interval to minInterval if applicable
        value = std::max(value, minInterval);
    }
    else
    {
        // Having a timer but not displaying an enabled value means we
        // are inside of the fallback
        value = fallback->interval;
    }

    // Update new expiration
    timer.setRemaining(milliseconds(value));

    // Update Base class data.
    return WatchdogInherits::timeRemaining(value);
}

// Set value of Interval
uint64_t Watchdog::interval(uint64_t value)
{
    return WatchdogInherits::interval(std::max(value, minInterval));
}

// Optional callback function on timer expiration
void Watchdog::timeOutHandler()
{
    if (timerEnabled())
    {
        timer.setEnabled(false);
    }
    const unsigned int instance = getInstanceFromObjectPath(objPath);
    const auto restartBusName = buildBusName(restart::busNameBase, instance);
    const auto restartPath =
        buildPath(restart::pathPrefix, instance, restart::pathSuffix);
    const auto chassisBusName = buildBusName(chassis::busNameBase, instance);
    const auto chassisPath =
        buildPath(chassis::pathPrefix, instance, chassis::pathSuffix);
    const auto hostBusName = buildBusName(host::busNameBase, instance);
    const auto hostPath = buildPath(host::pathPrefix, instance, host::pathSuffix);
    const auto nmiBusName = buildBusName(nmi::busNameBase, instance);
    const auto nmiPath = buildPath(nmi::pathPrefix, instance, nmi::pathSuffix);

    PreTimeoutInterruptAction preTimeoutInterruptAction = preTimeoutInterrupt();
    std::string preInterruptActionMessageArgs{};

    Action action = expireAction();
    std::string actionMessageArgs{};

    expiredTimerUse(currentTimerUse());

    TimerUse timeUser = expiredTimerUse();
    std::string timeUserMessage{};

    if (!this->enabled())
    {
        action = fallback->action;
    }

    switch (timeUser)
    {
        case Watchdog::TimerUse::BIOSFRB2:
            timeUserMessage = timerUseDescriptionBIOSFRB2;
            break;
        case Watchdog::TimerUse::BIOSPOST:
            timeUserMessage = timerUseDescriptionBIOSPOST;
            break;
        case Watchdog::TimerUse::OSLoad:
            timeUserMessage = timerUseDescriptionOSLoad;
            break;
        case Watchdog::TimerUse::SMSOS:
            timeUserMessage = timerUseDescriptionSMSOS;
            break;
        case Watchdog::TimerUse::OEM:
            timeUserMessage = timerUseDescriptionOEM;
            break;
        default:
            timeUserMessage = reservedDescription;
            break;
    }

    switch (action)
    {
        case Watchdog::Action::HardReset:
            actionMessageArgs = std::string(hardResetDescription) +
                                std::string(actionDescription);
            break;
        case Watchdog::Action::PowerOff:
            actionMessageArgs = std::string(powerOffDescription) +
                                std::string(actionDescription);
            break;
        case Watchdog::Action::PowerCycle:
            actionMessageArgs = std::string(powerCycleDescription) +
                                std::string(actionDescription);
            break;
        case Watchdog::Action::None:
            actionMessageArgs = timerExpiredDescription;
            break;
        default:
            actionMessageArgs = reservedDescription;
            break;
    }

    // Log into redfish event log
    sd_journal_send("MESSAGE=IPMIWatchdog: Timed out ACTION=%s",
                    convertForMessage(action).c_str(), "PRIORITY=%i", LOG_INFO,
                    "REDFISH_MESSAGE_ID=%s", "OpenBMC.0.1.IPMIWatchdog",
                    "REDFISH_MESSAGE_ARGS=%s. timer use: %s",
                    actionMessageArgs.c_str(), timeUserMessage.c_str(), NULL);

    switch (preTimeoutInterruptAction)
    {
        case Watchdog::PreTimeoutInterruptAction::SMI:
            preInterruptActionMessageArgs = preInterruptDescriptionSMI;
            break;
        case Watchdog::PreTimeoutInterruptAction::NMI:
            preInterruptActionMessageArgs = preInterruptDescriptionNMI;
            break;
        case Watchdog::PreTimeoutInterruptAction::MI:
            preInterruptActionMessageArgs = preInterruptDescriptionMI;
            break;
        default:
            preInterruptActionMessageArgs = reservedDescription;
            break;
    }

    if (preInterruptActionNone != convertForMessage(preTimeoutInterruptAction))
    {
        preTimeoutInterruptOccurFlag(true);

        sd_journal_send("MESSAGE=IPMIWatchdog: Pre Timed out Interrupt=%s",
                        convertForMessage(preTimeoutInterruptAction).c_str(),
                        "PRIORITY=%i", LOG_INFO, "REDFISH_MESSAGE_ID=%s",
                        "OpenBMC.0.1.IPMIWatchdog",
                        "REDFISH_MESSAGE_ARGS=Timer interrupt - %s due to "
                        "Watchdog timeout. timer use: %s",
                        preInterruptActionMessageArgs.c_str(),
                        timeUserMessage.c_str(), NULL);

        if (preTimeoutInterruptAction ==
            Watchdog::PreTimeoutInterruptAction::NMI)
        {
            sdbusplus::message::message preTimeoutInterruptHandler;
            preTimeoutInterruptHandler = bus.new_method_call(
                nmiBusName.c_str(), nmiPath.c_str(), nmi::interface,
                nmi::request);
            bus.call_noreply(preTimeoutInterruptHandler);
        }
    }

    auto target = actionTargetMap.find(action);
    if (target == actionTargetMap.end())
    {
        log<level::INFO>("watchdog: Timed out with no target",
                         entry("ACTION=%s", convertForMessage(action).c_str()),
                         entry("TIMER_USE=%s",
                               convertForMessage(expiredTimerUse()).c_str()));
    }
    else
    {
        log<level::INFO>(
            "watchdog: Timed out",
            entry("ACTION=%s", convertForMessage(action).c_str()),
            entry("TIMER_USE=%s", convertForMessage(expiredTimerUse()).c_str()),
            entry("TARGET=%s", target->second.c_str()));

        try
        {
            PDK_WatchdogAction();
            sdbusplus::message::message method;
            if (action == Watchdog::Action::HardReset)
            {
                auto method = bus.new_method_call(
                    restartBusName.c_str(), restartPath.c_str(),
                    "org.freedesktop.DBus.Properties", "Set");
                method.append(
                    restart::interface, restart::property,
                    std::variant<std::string>("xyz.openbmc_project.State.Host."
                                              "RestartCause.WatchdogTimer"));
                bus.call_noreply(method);
                method = bus.new_method_call(hostBusName.c_str(),
                                             hostPath.c_str(),
                                             "org.freedesktop.DBus.Properties",
                                             "Set");
                method.append(host::interface, host::request,
                              std::variant<std::string>(target->second));
                bus.call_noreply(method);
            }
            else
            {
                if ((action == Watchdog::Action::PowerCycle) ||
                    (action == Watchdog::Action::PowerOff))
                {
                    auto method = bus.new_method_call(
                        restartBusName.c_str(), restartPath.c_str(),
                        "org.freedesktop.DBus.Properties", "Set");
                    method.append(restart::interface, restart::property,
                                  std::variant<std::string>(
                                      "xyz.openbmc_project.State.Host."
                                      "RestartCause.WatchdogTimer"));
                    bus.call_noreply(method);
                }
                method = bus.new_method_call(chassisBusName.c_str(),
                                             chassisPath.c_str(),
                                             "org.freedesktop.DBus.Properties",
                                             "Set");
                method.append(chassis::interface, chassis::request,
                              std::variant<std::string>(target->second));
                bus.call_noreply(method);
            }
        }
        catch (const sdbusplus::exception_t& e)
        {
            log<level::ERR>("watchdog: Failed to start unit",
                            entry("TARGET=%s", target->second.c_str()),
                            entry("ERROR=%s", e.what()));
            commit<InternalFailure>();
        }
    }
    try
    {
        auto signal = bus.new_signal(objPath.data(),
                                     "xyz.openbmc_project.Watchdog", "Timeout");
        signal.append(convertForMessage(action).c_str());
        signal.signal_send();
    }
    catch (const sdbusplus::exception_t& e)
    {
        log<level::ERR>("watchdog: failed to send timeout signal",
                        entry("ERROR=%s", e.what()));
    }

    if (exitAfterTimeout)
    {
        timer.get_event().exit(0);
    }

    tryFallbackOrDisable();
}

void Watchdog::tryFallbackOrDisable()
{
    // We only re-arm the watchdog if we were already enabled and have
    // a possible fallback
    if (fallback && (fallback->always || this->enabled()))
    {
        auto interval_ms = fallback->interval;
        timer.restart(milliseconds(interval_ms));
        log<level::INFO>("watchdog: falling back",
                         entry("INTERVAL=%llu", interval_ms));
    }
    else if (timerEnabled())
    {
        timer.setEnabled(false);

        log<level::INFO>("watchdog: disabled");
    }

    // Make sure we accurately reflect our enabled state to the
    // dbus interface.
    WatchdogInherits::enabled(false);
}

} // namespace watchdog
} // namespace phosphor
