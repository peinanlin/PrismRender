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
    std::filesystem::path image;
    std::filesystem::path report;
    double minimumMeanLuminance = 0.01;
    double minimumLuminanceDeviation = 0.005;
    double minimumVisiblePixelRatio = 0.05;
    bool enforce = false;
};

Options ParseOptions(const int argumentCount, char** arguments)
{
    if (argumentCount < 2)
    {
        throw std::invalid_argument(
            "Usage: PrismImageVisibilityCheck <image.bmp> [--report=<path>] "
            "[--enforce] [--mean=<value>] [--contrast=<value>] "
            "[--visible=<value>]");
    }
    Options options{};
    options.image = arguments[1];
    for (int index = 2; index < argumentCount; ++index)
    {
        const std::string argument(arguments[index]);
        const auto valueAfter = [&](const char* prefix)
        {
            return argument.substr(
                std::char_traits<char>::length(prefix));
        };
        if (argument == "--enforce") options.enforce = true;
        else if (argument.starts_with("--report="))
            options.report = valueAfter("--report=");
        else if (argument.starts_with("--mean="))
            options.minimumMeanLuminance =
                std::stod(valueAfter("--mean="));
        else if (argument.starts_with("--contrast="))
            options.minimumLuminanceDeviation =
                std::stod(valueAfter("--contrast="));
        else if (argument.starts_with("--visible="))
            options.minimumVisiblePixelRatio =
                std::stod(valueAfter("--visible="));
        else
            throw std::invalid_argument("Unknown option: " + argument);
    }
    return options;
}

void WriteMetrics(
    std::ostream& output,
    const Prism::Tools::ImageVisibilityMetrics& metrics)
{
    output << std::fixed << std::setprecision(6)
           << "valid=" << (metrics.valid ? "true" : "false") << '\n'
           << "mean_luminance=" << metrics.meanLuminance << '\n'
           << "luminance_standard_deviation="
           << metrics.luminanceStandardDeviation << '\n'
           << "visible_pixel_ratio=" << metrics.visiblePixelRatio << '\n';
}
} // namespace

int main(const int argumentCount, char** arguments)
{
    try
    {
        const Options options = ParseOptions(argumentCount, arguments);
        Prism::Tools::ImageRgba8 image;
        std::string error;
        if (!Prism::Tools::LoadBitmapRgba8(
                options.image, image, &error))
        {
            throw std::runtime_error(error);
        }
        const Prism::Tools::ImageVisibilityMetrics metrics =
            Prism::Tools::AnalyzeImageVisibility(image);
        WriteMetrics(std::cout, metrics);
        if (!options.report.empty())
        {
            std::ofstream report(options.report, std::ios::trunc);
            if (!report)
            {
                throw std::runtime_error(
                    "Failed to open the report output path.");
            }
            WriteMetrics(report, metrics);
        }
        const bool passed = metrics.valid
            && metrics.meanLuminance
                >= options.minimumMeanLuminance
            && metrics.luminanceStandardDeviation
                >= options.minimumLuminanceDeviation
            && metrics.visiblePixelRatio
                >= options.minimumVisiblePixelRatio;
        return options.enforce && !passed
            ? EXIT_FAILURE
            : EXIT_SUCCESS;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "Image visibility check failed: "
                  << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
