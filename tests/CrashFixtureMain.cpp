#include "Core/ProcessDiagnostics.h"

#include <Windows.h>

namespace
{
__declspec(noinline) void TriggerTestCrash()
{
    volatile int* invalidAddress = nullptr;
    *invalidAddress = 17;
}
} // namespace

int main()
{
    SetErrorMode(
        SEM_FAILCRITICALERRORS
        | SEM_NOGPFAULTERRORBOX
        | SEM_NOOPENFILEERRORBOX);
    Prism::Core::ProcessDiagnostics::Initialize();
    Prism::Core::ProcessDiagnostics::SetGraphicsApi(
        "diagnostics-test");
    Prism::Core::ProcessDiagnostics::Log(
        "info",
        "diagnostics.fixture_start",
        "The crash fixture is about to raise an access violation.");
    TriggerTestCrash();
    return 0;
}
