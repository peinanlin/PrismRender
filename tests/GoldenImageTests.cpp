#include "Tools/GoldenImageComparator.h"

#include <cmath>
#include <iostream>

namespace
{
bool NearlyEqual(const double lhs, const double rhs)
{
    return std::abs(lhs - rhs) < 1.0e-9;
}
}

int main()
{
    Prism::Tools::ImageRgba8 reference{
        2, 1,
        {0, 0, 0, 255, 10, 20, 30, 255}};
    Prism::Tools::ImageRgba8 identical = reference;
    const Prism::Tools::GoldenImageMetrics exact =
        Prism::Tools::CompareGoldenImages(reference, identical);
    if (!exact.dimensionsMatch || !NearlyEqual(exact.meanAbsoluteError, 0.0)
        || !NearlyEqual(exact.rootMeanSquareError, 0.0)
        || !NearlyEqual(exact.changedPixelRatio, 0.0)
        || !NearlyEqual(exact.structuralSimilarity, 1.0))
    {
        std::cerr << "Exact golden image comparison failed.\n";
        return 1;
    }

    Prism::Tools::ImageRgba8 changed = reference;
    changed.pixels[0] = 255;
    const Prism::Tools::GoldenImageMetrics difference =
        Prism::Tools::CompareGoldenImages(reference, changed, 8);
    if (!NearlyEqual(difference.meanAbsoluteError, 1.0 / 6.0)
        || !NearlyEqual(difference.rootMeanSquareError, std::sqrt(1.0 / 6.0))
        || !NearlyEqual(difference.changedPixelRatio, 0.5)
        || !NearlyEqual(difference.maximumChannelError, 1.0)
        || difference.structuralSimilarity >= 1.0
        || difference.structuralSimilarity < 0.0)
    {
        std::cerr << "Golden image metric calculation failed.\n";
        return 1;
    }

    changed.width = 1;
    if (Prism::Tools::CompareGoldenImages(reference, changed).dimensionsMatch)
    {
        std::cerr << "Dimension mismatch was not detected.\n";
        return 1;
    }

    const Prism::Tools::ImageRgba8 visibleImage{
        2, 1,
        {0, 0, 0, 255, 255, 255, 255, 255}};
    const Prism::Tools::ImageVisibilityMetrics visibility =
        Prism::Tools::AnalyzeImageVisibility(visibleImage);
    if (!visibility.valid
        || !NearlyEqual(visibility.meanLuminance, 0.5)
        || !NearlyEqual(
            visibility.luminanceStandardDeviation,
            0.5)
        || !NearlyEqual(visibility.visiblePixelRatio, 0.5))
    {
        std::cerr << "Image visibility analysis failed.\n";
        return 1;
    }
    return 0;
}
