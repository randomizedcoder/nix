#include "nix/store/build/build-log.hh"

#include <nlohmann/json.hpp>

namespace nix {

BuildLog::BuildLog(size_t maxTailLines, std::unique_ptr<Activity> act)
    : maxTailLines(maxTailLines)
    , act(std::move(act))
{
}

void BuildLog::operator()(std::string_view data)
{
    for (auto c : data)
        if (c == '\r')
            currentLogLinePos = 0;
        else if (c == '\n')
            flushLine();
        else {
            if (currentLogLinePos >= currentLogLine.size())
                currentLogLine.resize(currentLogLinePos + 1);
            currentLogLine[currentLogLinePos++] = c;
        }
}

void BuildLog::flush()
{
    if (!currentLogLine.empty())
        flushLine();
}

void BuildLog::flushLine()
{
    // Truncate to actual content (currentLogLinePos may be less than size due to \r)
    currentLogLine.resize(currentLogLinePos);

    // Pre-parse JSON to detect setPhase before handleJSONLogMessage
    if (onPhaseChange) {
        auto json = parseJSONMessage(currentLogLine, "the derivation builder");
        if (json) {
            auto it = json->find("action");
            if (it != json->end() && *it == "setPhase") {
                auto phaseIt = json->find("phase");
                if (phaseIt != json->end() && phaseIt->is_string())
                    onPhaseChange(phaseIt->get<std::string>());
            }
        }
    }

    if (!handleJSONLogMessage(currentLogLine, *act, builderActivities, "the derivation builder", false)) {
        // Line was not handled as JSON, emit and add to tail
        act->result(resBuildLogLine, currentLogLine);
        logTail.push_back(currentLogLine);
        if (logTail.size() > maxTailLines)
            logTail.pop_front();
    }

    currentLogLine.clear();
    currentLogLinePos = 0;
}

} // namespace nix
