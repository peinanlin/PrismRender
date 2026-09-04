#include "Tools/FrameDiagnosticsComparison.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv)
{
    try
    {
        if (argc != 4) throw std::runtime_error("Usage: PrismFrameDiagnosticsCompare reference.json candidate.json report.json");
        if (std::filesystem::exists(argv[3])) throw std::runtime_error("Refusing to overwrite comparison report.");
        std::ifstream reference(argv[1]);
        std::ifstream candidate(argv[2]);
        const auto report = Prism::Tools::CompareFrameDiagnostics(
            nlohmann::json::parse(reference), nlohmann::json::parse(candidate));
        std::ofstream output(argv[3]);
        output << report.dump(2) << '\n';
        output.close();
        if (!output) throw std::runtime_error("Cannot write comparison report.");
        std::cout << report.dump(2) << '\n';
        return report.at("passed").get<bool>() ? 0 : 1;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
