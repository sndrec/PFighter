#include "core/simulation.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>
#include <cctype>

namespace {

void printUsage() {
    std::cerr
        << "usage: pfighter_package_converter --fighter <name> --out <package.pfpkg>\n"
        << "       pfighter_package_converter --all --out-dir <directory>\n"
        << "       pfighter_package_converter --list\n";
}

std::string nativePackageFileStem(std::string name) {
    std::string out;
    out.reserve(name.size() + 7);
    bool previousUnderscore = false;
    for (char ch : name) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (std::isalnum(c)) {
            out.push_back(static_cast<char>(std::tolower(c)));
            previousUnderscore = false;
        } else if (!previousUnderscore && !out.empty()) {
            out.push_back('_');
            previousUnderscore = true;
        }
    }
    while (!out.empty() && out.back() == '_') {
        out.pop_back();
    }
    if (out.empty()) {
        out = "fighter";
    }
    out += "_native";
    return out;
}

} // namespace

int main(int argc, char** argv) {
    std::string fighterName;
    std::string outputPath;
    std::string outputDir;
    bool list = false;
    bool all = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--list") {
            list = true;
        } else if (arg == "--all") {
            all = true;
        } else if (arg == "--fighter" && i + 1 < argc) {
            fighterName = argv[++i];
        } else if (arg == "--out" && i + 1 < argc) {
            outputPath = argv[++i];
        } else if (arg == "--out-dir" && i + 1 < argc) {
            outputDir = argv[++i];
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

    if (all) {
        if (!fighterName.empty() || !outputPath.empty() || outputDir.empty()) {
            printUsage();
            return 2;
        }
        std::error_code ec;
        std::filesystem::create_directories(outputDir, ec);
        if (ec) {
            std::cerr << "failed to create output directory '" << outputDir << "': " << ec.message() << '\n';
            return 1;
        }

        bool ok = true;
        for (const std::string& name : pf::meleeTrainingRosterFighterNames()) {
            const std::filesystem::path path = std::filesystem::path(outputDir) / (nativePackageFileStem(name) + ".pfpkg");
            std::string error;
            if (!pf::saveConvertedMeleeFighterPackage(name, path.string(), &error)) {
                std::cerr << "conversion failed for " << name << ": " << error << '\n';
                ok = false;
                continue;
            }
            std::cout << "wrote native fighter package: " << path.string() << '\n';
        }
        return ok ? 0 : 1;
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
