#include "UI/PerformanceProfilerPanelModel.h"

#include <iostream>
#include <stdexcept>
#include <vector>

namespace
{
void Expect(const bool condition, const char* const message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}
} // namespace

int main()
{
    using Prism::Core::FrameProfilerSnapshot;
    using Prism::UI::PerformanceProfilerPanelModel;
    try
    {
        Expect(!PerformanceProfilerPanelModel::ShouldReadCompletedFrame(
                   false, false)
                && !PerformanceProfilerPanelModel::ShouldReadCompletedFrame(
                    true, false)
                && PerformanceProfilerPanelModel::ShouldReadCompletedFrame(
                    true, true),
            "Collapsed panel requested a completed snapshot.");
        Expect(!PerformanceProfilerPanelModel::ShouldReadHistory(
                   true, true, false)
                && !PerformanceProfilerPanelModel::ShouldReadHistory(
                    true, false, true)
                && PerformanceProfilerPanelModel::ShouldReadHistory(
                    true, true, true),
            "Collapsed timeline requested history construction.");

        std::vector<FrameProfilerSnapshot> history(245);
        for (std::size_t index = 0; index < history.size(); ++index)
        {
            history[index].editorLoop = {
                static_cast<double>(index + 1), true};
        }
        history[10].editorLoop.available = false;
        const auto timeline =
            PerformanceProfilerPanelModel::BuildTimeline(history);
        Expect(timeline.count == 240,
            "Panel timeline exceeded its bounded capacity.");
        Expect(timeline.editorLoopMilliseconds.front() == 6.0f
                && timeline.editorLoopMilliseconds[5] == 0.0f
                && timeline.editorLoopMilliseconds.back() == 245.0f,
            "Panel timeline did not retain the most recent ordered samples.");

        history.resize(12);
        const auto unavailable =
            PerformanceProfilerPanelModel::BuildTimeline(history);
        Expect(unavailable.count == 12
                && unavailable.editorLoopMilliseconds[10] == 0.0f,
            "Unavailable panel duration was not represented explicitly.");
        std::cout << "Performance profiler panel model tests passed.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
