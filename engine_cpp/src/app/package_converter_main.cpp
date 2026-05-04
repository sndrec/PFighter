#include "core/melee_fighter_converter.hpp"

#include <chrono>
#include <cstdlib>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

void printUsage() {
    std::cerr
        << "usage: pfighter_package_converter --fighter <name> --out <package.pfpkg>\n"
        << "       pfighter_package_converter --fighter-dat <Pl*.dat> --costume-dat <Pl*Nr.dat> --name <display> --out <package.pfpkg>\n"
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

std::filesystem::path defaultExporterProjectPath() {
    const std::vector<std::filesystem::path> candidates = {
        "engine_cpp/tools/hsd_exporter/PFighter.HsdExporter.csproj",
        "tools/hsd_exporter/PFighter.HsdExporter.csproj",
        "../tools/hsd_exporter/PFighter.HsdExporter.csproj",
        "../../engine_cpp/tools/hsd_exporter/PFighter.HsdExporter.csproj",
        "../../tools/hsd_exporter/PFighter.HsdExporter.csproj",
    };
    for (const std::filesystem::path& candidate : candidates) {
        if (std::filesystem::exists(candidate)) {
            return candidate;
        }
    }
    return candidates.front();
}

std::string quoteArg(const std::filesystem::path& path) {
    std::string value = path.string();
    std::string quoted = "\"";
    for (char ch : value) {
        if (ch == '"') {
            quoted += "\\\"";
        } else {
            quoted += ch;
        }
    }
    quoted += "\"";
    return quoted;
}

std::filesystem::path temporaryAssetBinPath() {
    const auto ticks = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
        ("pfighter_import_" + std::to_string(static_cast<long long>(ticks)) + ".pfighter.bin");
}

bool exportDatToTemporaryAssetBin(
    const std::filesystem::path& exporterProject,
    const std::filesystem::path& fighterDat,
    const std::filesystem::path& costumeDat,
    const std::filesystem::path& assetBin)
{
    const std::string command =
        "dotnet run --project " + quoteArg(exporterProject) +
        " -- --asset-bin-out " + quoteArg(assetBin) +
        " " + quoteArg(fighterDat) +
        " " + quoteArg(costumeDat);
    return std::system(command.c_str()) == 0;
}

} // namespace

int main(int argc, char** argv) {
    std::string fighterName;
    std::string outputPath;
    std::string outputDir;
    std::string fighterDatPath;
    std::string costumeDatPath;
    std::string displayName;
    std::string exporterProjectPath;
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
        } else if (arg == "--fighter-dat" && i + 1 < argc) {
            fighterDatPath = argv[++i];
        } else if (arg == "--costume-dat" && i + 1 < argc) {
            costumeDatPath = argv[++i];
        } else if (arg == "--name" && i + 1 < argc) {
            displayName = argv[++i];
        } else if (arg == "--exporter-project" && i + 1 < argc) {
            exporterProjectPath = argv[++i];
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

    const bool datImport = !fighterDatPath.empty() || !costumeDatPath.empty() || !displayName.empty() || !exporterProjectPath.empty();
    if (datImport) {
        if (!fighterName.empty() || fighterDatPath.empty() || costumeDatPath.empty() || displayName.empty() || outputPath.empty() ||
            !outputDir.empty())
        {
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

        const std::filesystem::path exporterProject =
            exporterProjectPath.empty() ? defaultExporterProjectPath() : std::filesystem::path(exporterProjectPath);
        const std::filesystem::path assetBin = temporaryAssetBinPath();
        if (!exportDatToTemporaryAssetBin(exporterProject, fighterDatPath, costumeDatPath, assetBin)) {
            std::filesystem::remove(assetBin, ec);
            std::cerr << "DAT import failed while exporting PFighter scratch asset\n";
            return 1;
        }

        std::string error;
        const std::string sourceFileName =
            std::filesystem::path(fighterDatPath).filename().string() + "+" +
            std::filesystem::path(costumeDatPath).filename().string();
        const bool ok = pf::saveConvertedMeleeFighterPackageFromAssetBin(
            displayName,
            sourceFileName,
            assetBin.string(),
            outputPath,
            &error);
        std::filesystem::remove(assetBin, ec);
        if (!ok) {
            std::cerr << "conversion failed: " << error << '\n';
            return 1;
        }

        std::cout << "wrote native fighter package: " << outputPath << '\n';
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
