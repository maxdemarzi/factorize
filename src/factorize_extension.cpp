#define DUCKDB_EXTENSION_MAIN

#include "factorize_extension.hpp"

#include "factorize/optimizer_rule.hpp"
#include "duckdb/main/config.hpp"

namespace duckdb {

static void LoadInternal(ExtensionLoader &loader) {
	auto &db = loader.GetDatabaseInstance();
	auto &config = DBConfig::GetConfig(db);
	FactorizeOptimizerExtension::Register(config);
}

void FactorizeExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string FactorizeExtension::Name() {
	return "factorize";
}

std::string FactorizeExtension::Version() const {
#ifdef EXT_VERSION_FACTORIZE
	return EXT_VERSION_FACTORIZE;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(factorize, loader) {
	duckdb::LoadInternal(loader);
}
}
