#include "Renderer/Features/FeatureGraphTypes.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace
{
using namespace Prism::Renderer;

struct TestSlot
{
};
struct MissingSlot
{
};
struct TestValue
{
    int value = 0;
};
struct WrongValue
{
    int value = 0;
};

void Expect(const bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

template <typename Callback>
void ExpectFailureContaining(
    Callback&& callback,
    const std::string& expectedText)
{
    try
    {
        std::forward<Callback>(callback)();
    }
    catch (const std::exception& exception)
    {
        Expect(
            std::string(exception.what()).find(expectedText)
                != std::string::npos,
            "Feature graph failure did not report the expected reason.");
        return;
    }
    throw std::runtime_error("Expected feature graph contract failure.");
}

RenderGraphBlackboardPublication Publication(
    std::string producer,
    const std::uint32_t generation,
    const std::uint32_t version,
    const std::uint64_t viewId = 7)
{
    return {
        std::move(producer),
        RenderGraphBlackboardValueScope::ViewLocal,
        viewId,
        generation,
        version};
}

RenderGraphBlackboardRequest Request(
    const std::uint32_t generation,
    const std::uint64_t viewId = 7,
    const std::optional<std::uint32_t> version = std::nullopt)
{
    return {
        "test-consumer",
        viewId,
        generation,
        version,
        "Test.FeatureInput"};
}

void TestTypedPublicationValidation()
{
    RenderGraphBlackboard blackboard;
    blackboard.Publish<TestSlot>(
        TestValue{42}, Publication("producer", 4, 2));
    Expect(
        blackboard.Require<TestSlot, TestValue>(Request(4, 7, 2)).value
            == 42,
        "Typed feature graph publication was not preserved.");

    ExpectFailureContaining(
        [&blackboard]()
        {
            static_cast<void>(
                blackboard.Require<MissingSlot, TestValue>(Request(4)));
        },
        "missing");
    ExpectFailureContaining(
        [&blackboard]()
        {
            static_cast<void>(
                blackboard.Require<MissingSlot, TestValue>(Request(4)));
        },
        "Test.FeatureInput");
    ExpectFailureContaining(
        [&blackboard]()
        {
            static_cast<void>(
                blackboard.Require<TestSlot, WrongValue>(Request(4)));
        },
        "wrong value type");
    ExpectFailureContaining(
        [&blackboard]()
        {
            blackboard.Publish<TestSlot>(
                TestValue{9}, Publication("other-producer", 4, 3));
        },
        "conflicting producer");
    ExpectFailureContaining(
        [&blackboard]()
        {
            static_cast<void>(
                blackboard.Require<TestSlot, TestValue>(Request(5)));
        },
        "stale graph generation");
    ExpectFailureContaining(
        [&blackboard]()
        {
            static_cast<void>(
                blackboard.Require<TestSlot, TestValue>(Request(4, 7, 1)));
        },
        "expected version");
    ExpectFailureContaining(
        [&blackboard]()
        {
            static_cast<void>(
                blackboard.Require<TestSlot, TestValue>(Request(4, 8, 2)));
        },
        "cannot cross views");
}

void TestExplicitVersionAndOptionalFallback()
{
    RenderGraphBlackboard blackboard;
    const TestValue fallback{17};
    const TestValue selected =
        blackboard.GetOptionalOr<MissingSlot, TestValue>(
            Request(9), fallback);
    Expect(
        selected.value == fallback.value,
        "Optional input did not use its explicit fallback.");

    blackboard.Publish<TestSlot>(
        TestValue{1}, Publication("producer", 9, 1));
    blackboard.PublishNext<TestSlot>(
        TestValue{2}, Publication("producer", 9, 2));
    Expect(
        blackboard.Require<TestSlot, TestValue>(Request(9, 7, 2)).value
            == 2,
        "Explicit next publication did not replace the prior version.");
    ExpectFailureContaining(
        [&blackboard]()
        {
            blackboard.PublishNext<TestSlot>(
                TestValue{3}, Publication("producer", 9, 2));
        },
        "must advance explicitly");
    ExpectFailureContaining(
        [&blackboard]()
        {
            blackboard.PublishNext<TestSlot>(
                TestValue{3}, Publication("other", 9, 3));
        },
        "different producer");

    blackboard.Clear();
    ExpectFailureContaining(
        [&blackboard]()
        {
            static_cast<void>(
                blackboard.Require<TestSlot, TestValue>(Request(9)));
        },
        "missing");
}
} // namespace

int main()
{
    try
    {
        TestTypedPublicationValidation();
        TestExplicitVersionAndOptionalFallback();
        std::cout << "Feature graph contract tests passed.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "Feature graph contract tests failed: "
                  << exception.what() << '\n';
        return 1;
    }
}
