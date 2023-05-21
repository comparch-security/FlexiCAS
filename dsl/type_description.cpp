#include "dsl/type_description.hpp"
#include "dsl/statement.hpp"

DescriptionDB::DescriptionDB() {
  Description *descriptor;
  // adding types with no template parameters
  descriptor = new TypeData64B(); add("Data64B", descriptor); descriptor->emit_header();
}

DescriptionDB::~DescriptionDB() {
  for(auto t:types) delete t.second;
}

bool DescriptionDB::create(const std::string &type_name, const std::string &base_name, std::list<std::string> &params) {
  Description *descriptor;
  if(base_name == "MetadataMSI") descriptor = new TypeMetadataMSI(type_name);

  if(!descriptor->set(params)) return false;
  add(type_name, descriptor);
  descriptor->emit_header();
  codegendb.type_declarations.push_back(descriptor);
  return true;
}

DescriptionDB typedb;

void Description::emit_header() { codegendb.add_header("cache/cache.hpp"); }
void TypeMetadataMSI::emit_header() { codegendb.add_header("cache/msi.hpp"); }
void TypeOuterPortMSIUncached::emit_header() { codegendb.add_header("cache/msi.hpp"); }
void TypeOuterPortMSI::emit_header() { codegendb.add_header("cache/msi.hpp"); }
void TypeInnerPortMSIBroadcast::emit_header() { codegendb.add_header("cache/msi.hpp"); }
void TypeCoreInterfaceMSI::emit_header() { codegendb.add_header("cache/msi.hpp"); }
void TypeCoherentCacheBase::emit_header() { codegendb.add_header("cache/coherence.hpp"); }
void TypeIndexFuncBase::emit_header() { codegendb.add_header("cache/index.hpp"); }
void TypeReplaceFuncBase::emit_header() { codegendb.add_header("cache/replace.hpp"); }
