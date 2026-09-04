#include "Tools/GoldenImageComparator.h"

#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
struct Options
{
    std::filesystem::path reference;
    std::filesystem::path candidate;
    std::filesystem::path report;
    double meanThreshold = 0.12;
    double rmseThreshold = 0.20;
    double changedThreshold = 0.45;
    double minimumSsim = 0.0;
    std::uint8_t pixelTolerance = 8;
    bool enforce = false;
};

Options ParseOptions(const int argumentCount, char** arguments)
{
    if (argumentCount < 3)
    {
        throw std::invalid_argument(
            "Usage: PrismGoldenImageCompare <reference.bmp> <candidate.bmp> "
            "[--report=<path>] [--enforce] [--mean=<value>] [--rmse=<value>] "
            "[--changed=<value>] [--ssim=<value>] "
            "[--pixel-tolerance=<0..255>]");
    }
    Options options{};
    options.reference = arguments[1];
    options.candidate = arguments[2];
    for (int index = 3; index < argumentCount; ++index)
    {
        const std::string argument(arguments[index]);
        const auto valueAfter = [&](const char* prefix)
        {
            return argument.substr(std::char_traits<char>::length(prefix));
        };
        if (argument == "--enforce") options.enforce = true;
        else if (argument.starts_with("--report=")) options.report = valueAfter("--report=");
        else if (argument.starts_with("--mean=")) options.meanThreshold = std::stod(valueAfter("--mean="));
        else if (argument.starts_with("--rmse=")) options.rmseThreshold = std::stod(valueAfter("--rmse="));
        else if (argument.starts_with("--changed=")) options.changedThreshold = std::stod(valueAfter("--changed="));
        else if (argument.starts_with("--ssim=")) options.minimumSsim = std::stod(valueAfter("--ssim="));
        else if (argument.starts_with("--pixel-tolerance="))
        {
            const int tolerance = std::stoi(valueAfter("--pixel-tolerance="));
            if (tolerance < 0 || tolerance > 255)
            {
                throw std::invalid_argument("Pixel tolerance must be between 0 and 255.");
            }
            options.pixelTolerance = static_cast<std::uint8_t>(tolerance);
        }
        else throw std::invalid_argument("Unknown option: " + argument);
    }
    return options;
}

void WriteMetrics(std::ostream& output, const Prism::Tools::GoldenImageMetrics& metrics)
{
    output << std::fixed << std::setprecision(6)
           << "dimensions_match=" << (metrics.dimensionsMatch ? "true" : "false") << '\n'
           << "mean_absolute_error=" << metrics.meanAbsoluteError << '\n'
           << "root_mean_square_error=" << metrics.rootMeanSquareError << '\n'
           << "changed_pixel_ratio=" << metrics.changedPixelRatio << '\n'
           << "maximum_channel_error=" << metrics.maximumChannelError << '\n'
           << "structural_similarity=" << metrics.structuralSimilarity << '\n';
}
} // namespace

int main(const int argumentCount, char** arguments)
{
    try
    {
        const Options options = ParseOptions(argumentCount, arguments);
        Prism::Tools::ImageRgba8 reference;
        Prism::Tools::ImageRgba8 candidate;
        std::string error;
        if (!Prism::Tools::LoadBitmapRgba8(options.reference, reference, &error))
        {
            throw std::runtime_error("Reference image: " + error);
        }
        if (!Prism::Tools::LoadBitmapRgba8(options.candidate, candidate, &error))
        {
            throw std::runtime_error("Candidate image: " + error);
        }
        const Prism::Tools::GoldenImageMetrics metrics =
            Prism::Tools::CompareGoldenImages(reference, candidate, options.pixelTolerance);
        WriteMetrics(std::cout, metrics);
        if (!options.report.empty())
        {
            std::ofstream report(options.report, std::ios::trunc);
            if (!report) throw std::runtime_error("Failed to open the report output path.");
            WriteMetrics(report, metrics);
        }
        const bool passed = metrics.dimensionsMatch
            && metrics.meanAbsoluteError <= options.meanThreshold
            && metrics.rootMeanSquareError <= options.rmseThreshold
            && metrics.changedPixelRatio <= options.changedThreshold
            && metrics.structuralSimilarity >= options.minimumSsim;
        return options.enforce && !passed ? EXIT_FAILURE : EXIT_SUCCESS;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "Golden image comparison failed: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
