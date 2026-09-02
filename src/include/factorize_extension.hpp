//===----------------------------------------------------------------------===//
//                         factorize
//
// factorize_extension.hpp
//
// Factorized execution for DuckDB: keeps many-to-many join intermediates in
// f-representations (Lehner & Neumann, PVLDB 19(11):3006-3019) instead of
// materializing the flat blow-up.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.hpp"

namespace duckdb {

class FactorizeExtension : public Extension {
public:
	void Load(ExtensionLoader &loader) override;
	std::string Name() override;
	std::string Version() const override;
};

} // namespace duckdb
