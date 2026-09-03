//===----------------------------------------------------------------------===//
//                         factorize
//
// factorize/optimizer_rule.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/optimizer/optimizer_extension.hpp"

namespace duckdb {

//! How aggressively the extension takes over plans.
enum class FactorizeMode : uint8_t {
	//! Never fire. Also the A/B reference for correctness runs.
	OFF,
	//! Intended to fire when the matcher accepts and the cost gate agrees (plan
	//! Phase 5). The gate does not exist yet -- Phase 0/2's operator is a stub
	//! that returns a hardcoded constant, not a real count -- so until Phase 3
	//! lands a gate, AUTO behaves as OFF rather than as an unguarded FORCE.
	//! Behaving like FORCE was tried and reverted: the option's own
	//! documentation promises a distinction FORCE explicitly disclaims
	//! ("benchmarking only"), and honoring that promise by declining is safer
	//! than honoring it by fabricating a gate that isn't there.
	AUTO,
	//! Fire whenever the matcher accepts, ignoring the gate. Benchmarking only.
	FORCE
};

//! Reads the `factorize_mode` setting. Any unrecognised value is treated as OFF:
//! declining is always correct, taking over wrongly is not.
FactorizeMode GetFactorizeMode(ClientContext &context);

class FactorizeOptimizerExtension : public OptimizerExtension {
public:
	FactorizeOptimizerExtension();

	//! Registers the rule plus its settings on the given config.
	static void Register(DBConfig &config);

private:
	static void Optimize(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan);
};

} // namespace duckdb
