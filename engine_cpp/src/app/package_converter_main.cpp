#include "core/simulation.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

void printUsage() {
    std::cerr
        << "usage: pfighter_package_converter --fighter <name> --out <package.pfpkg>\n"
        << "       pfighter_package_converter --list\n";
}

} // namespace

int main(int argc, char** argv) {
    std::string fighterName;
    std::string outputPath;
    bool list = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--list") {
            list = true;
        } else if (arg == "--fighter" && i + 1 < argc) {
            fighterName = argv[++i];
        } else if (arg == "--out" && i + 1 < argc) {
            outputPath = argv[++i];
        } else {
            printUsage();
            return 2;
        }
    }

    if (list) {
        for (const std::string& name : pf::meleeTrainingRosterFighterNames()) {
            std::cout << name << '\n';
        }
        return 0;
    }

    if (fighterName.empty() || outputPath.empty()) {
        printUsage();
        return 2;
    }

    std::error_code ec;
    if (const std::filesystem::path parent = std::filesystem::path(outputPath).parent_path(); !parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            std::cerr << "failed to create output directory '" << parent.string() << "': " << ec.message() << '\n';
            return 1;
        }
    }

    std::string error;
    if (!pf::saveConvertedMeleeFighterPackage(fighterName, outputPath, &error)) {
        std::cerr << "conversion failed: " << error << '\n';
        return 1;
    }

    std::cout << "wrote native fighter package: " << outputPath << '\n';
    return 0;
}
