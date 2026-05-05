#include "core/action.hpp"

#include <algorithm>

namespace pf {

namespace {

bool isImportedTimingSubaction(SubactionType type) {
    return type == SubactionType::SyncTimer ||
        type == SubactionType::AsyncTimer ||
        type == SubactionType::SetLoop ||
        type == SubactionType::ExecuteLoop;
}

struct TimedSubaction {
    int frame = 0;
    int order = 0;
    Subaction subaction;
};

} // namespace

UnfoldedAction unfoldAction(const std::vector<Subaction>& action) {
    int maxFrame = 0;
    for (const Subaction& subaction : action) {
        if (isImportedTimingSubaction(subaction.type)) {
            continue;
        }
        maxFrame = std::max(maxFrame, subaction.startFrame);
    }

    UnfoldedAction unfolded(static_cast<size_t>(std::max(0, maxFrame) + 1));
    for (const Subaction& subaction : action) {
        if (isImportedTimingSubaction(subaction.type)) {
            continue;
        }
        const int frame = std::max(0, subaction.startFrame);
        unfolded[static_cast<size_t>(frame)].push_back(subaction);
    }
    return unfolded;
}

std::vector<int> subactionFirstFrames(const std::vector<Subaction>& action) {
    std::vector<int> frames(action.size(), -1);
    for (size_t index = 0; index < action.size(); ++index) {
        const Subaction& subaction = action[index];
        if (isImportedTimingSubaction(subaction.type)) {
            continue;
        }
        frames[index] = std::max(0, subaction.startFrame);
    }
    return frames;
}

std::vector<Subaction> makeExplicitTimelineAction(const std::vector<Subaction>& action) {
    const bool hasImportedTiming = std::any_of(action.begin(), action.end(), [](const Subaction& subaction) {
        return isImportedTimingSubaction(subaction.type);
    });
    if (!hasImportedTiming) {
        std::vector<Subaction> explicitAction = action;
        for (Subaction& subaction : explicitAction) {
            subaction.startFrame = std::max(0, subaction.startFrame);
        }
        std::stable_sort(explicitAction.begin(), explicitAction.end(), [](const Subaction& a, const Subaction& b) {
            return a.startFrame < b.startFrame;
        });
        return explicitAction;
    }

    std::vector<TimedSubaction> timed;
    timed.reserve(action.size());
    int currentFrame = 0;
    int loopCount = 0;
    size_t loopStart = 0;
    size_t index = 0;
    int order = 0;
    int safety = 0;
    while (index < action.size() && safety < 10000) {
        ++safety;
        const Subaction& subaction = action[index];
        if (subaction.type == SubactionType::SetLoop) {
            loopStart = index + 1;
            loopCount = std::max(0, subaction.loopCount - 1);
            ++index;
            continue;
        }
        if (subaction.type == SubactionType::ExecuteLoop) {
            if (loopCount > 0) {
                index = loopStart;
                --loopCount;
            } else {
                ++index;
            }
            continue;
        }
        if (subaction.type == SubactionType::SyncTimer) {
            currentFrame += std::max(0, subaction.frames);
            ++index;
            continue;
        }
        if (subaction.type == SubactionType::AsyncTimer) {
            currentFrame = std::max(currentFrame, std::max(0, subaction.frames));
            ++index;
            continue;
        }

        Subaction explicitSubaction = subaction;
        explicitSubaction.startFrame = currentFrame;
        timed.push_back({currentFrame, order++, std::move(explicitSubaction)});
        ++index;
    }

    std::stable_sort(timed.begin(), timed.end(), [](const TimedSubaction& a, const TimedSubaction& b) {
        if (a.frame != b.frame) {
            return a.frame < b.frame;
        }
        return a.order < b.order;
    });

    std::vector<Subaction> explicitAction;
    explicitAction.reserve(timed.size());
    for (TimedSubaction& event : timed) {
        explicitAction.push_back(std::move(event.subaction));
    }
    return explicitAction;
}

} // namespace pf
