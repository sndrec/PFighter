#include "core/action.hpp"

#include <algorithm>

namespace pf {

UnfoldedAction unfoldAction(const std::vector<Subaction>& action) {
    int subactionLength = 0;
    for (size_t i = 0; i < action.size(); ++i) {
        const Subaction& sub = action[i];
        if (sub.type == SubactionType::SetLoop) {
            constexpr int maxLoopSize = 100;
            int loopLength = 1;
            bool loopActive = true;
            while (loopActive && i + static_cast<size_t>(loopLength) < action.size()) {
                const Subaction& loopSub = action[i + static_cast<size_t>(loopLength)];
                if (loopSub.type == SubactionType::SyncTimer) {
                    subactionLength += loopSub.frames * std::max(0, sub.loopCount - 1);
                }
                if (loopSub.type == SubactionType::ExecuteLoop) {
                    loopActive = false;
                    i += static_cast<size_t>(loopLength);
                }
                ++loopLength;
                if (loopLength >= maxLoopSize) {
                    loopActive = false;
                    ++i;
                }
            }
        }
        if (sub.type == SubactionType::SyncTimer) {
            subactionLength += sub.frames;
            continue;
        }
        if (sub.type == SubactionType::AsyncTimer) {
            subactionLength = std::max(subactionLength, sub.frames);
            continue;
        }
    }

    UnfoldedAction unfolded(static_cast<size_t>(subactionLength + 1));
    int currentFrame = 0;
    int loopCount = 0;
    size_t loopStart = 0;
    size_t index = 0;
    while (index < action.size()) {
        const Subaction& sub = action[index];
        if (sub.type == SubactionType::SetLoop) {
            loopStart = index + 1;
            loopCount = std::max(0, sub.loopCount - 1);
            ++index;
            continue;
        }
        if (sub.type == SubactionType::ExecuteLoop) {
            if (loopCount > 0) {
                index = loopStart;
                --loopCount;
            } else {
                ++index;
            }
            continue;
        }
        if (sub.type == SubactionType::SyncTimer) {
            currentFrame += sub.frames;
            ++index;
            continue;
        }
        if (sub.type == SubactionType::AsyncTimer) {
            currentFrame = std::max(currentFrame, sub.frames);
            ++index;
            continue;
        }
        if (currentFrame >= 0 && currentFrame < static_cast<int>(unfolded.size())) {
            unfolded[static_cast<size_t>(currentFrame)].push_back(sub);
        }
        ++index;
    }

    return unfolded;
}

} // namespace pf
