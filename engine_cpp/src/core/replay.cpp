#include "core/replay.hpp"

#include <fstream>
#include <sstream>

namespace pf {
namespace {

void writeInput(std::ostream& out, const InputFrame& input) {
    out << input.move.x << ' ' << input.move.y << ' '
        << input.cStick.x << ' ' << input.cStick.y << ' '
        << input.shieldAnalog << ' ' << input.buttons;
}

bool readInput(std::istream& in, InputFrame& input) {
    int buttons = 0;
    if (!(in >> input.move.x >> input.move.y >>
          input.cStick.x >> input.cStick.y >>
          input.shieldAnalog >> buttons))
    {
        return false;
    }
    input.buttons = static_cast<uint16_t>(buttons);
    return true;
}

bool fail(std::string* error, const std::string& message) {
    if (error) {
        *error = message;
    }
    return false;
}

} // namespace

bool saveReplay(const std::string& path, const ReplayData& replay, std::string* error) {
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        return fail(error, "failed to open replay for writing: " + path);
    }

    out << "PFREPLAY 1\n";
    out << "fighters " << replay.p1FighterDef << ' ' << replay.p2FighterDef << '\n';
    out << "frames " << replay.frames.size() << '\n';
    for (const ReplayFrame& frame : replay.frames) {
        writeInput(out, frame.inputs[0]);
        out << " | ";
        writeInput(out, frame.inputs[1]);
        out << '\n';
    }

    if (!out) {
        return fail(error, "failed while writing replay: " + path);
    }
    return true;
}

bool loadReplay(const std::string& path, ReplayData& replay, std::string* error) {
    std::ifstream in(path);
    if (!in) {
        return fail(error, "failed to open replay for reading: " + path);
    }

    std::string magic;
    int version = 0;
    if (!(in >> magic >> version) || magic != "PFREPLAY" || version != 1) {
        return fail(error, "invalid replay header: " + path);
    }

    std::string label;
    ReplayData loaded;
    if (!(in >> label) || label != "fighters" ||
        !(in >> loaded.p1FighterDef >> loaded.p2FighterDef))
    {
        return fail(error, "invalid replay fighter metadata: " + path);
    }

    size_t frameCount = 0;
    if (!(in >> label) || label != "frames" || !(in >> frameCount)) {
        return fail(error, "invalid replay frame metadata: " + path);
    }

    loaded.frames.reserve(frameCount);
    for (size_t i = 0; i < frameCount; ++i) {
        ReplayFrame frame;
        char separator = 0;
        if (!readInput(in, frame.inputs[0]) || !(in >> separator) || separator != '|' ||
            !readInput(in, frame.inputs[1]))
        {
            std::ostringstream message;
            message << "invalid replay frame " << i << ": " << path;
            return fail(error, message.str());
        }
        loaded.frames.push_back(frame);
    }

    replay = std::move(loaded);
    return true;
}

} // namespace pf
