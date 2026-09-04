#pragma once

#include <json.hpp>

namespace Prism::Tools
{
// Does not compare pixels or certify driver validation. Backend identity in
// the reports selects strict same-backend or semantic cross-backend policy.
[[nodiscard]] nlohmann::json CompareFrameDiagnostics(
    const nlohmann::json& reference, const nlohmann::json& candidate);
} // namespace Prism::Tools
