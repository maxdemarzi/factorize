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
	//! Fire when the matcher accepts and the cost gate agrees (plan Phase 5).
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
