#include "dsl/type_description.hpp"
#include "dsl/statement.hpp"

void DescriptionDB::init() {
  Description *descriptor;
  // adding types with no template parameters
  descriptor = new TypeData64B(); add("Data64B", descriptor); descriptor->emit_header();
}

DescriptionDB::~DescriptionDB() {
  for(auto t:types) delete t.second;
}

bool DescriptionDB::create(const std::string &type_name, const std::string &base_name, std::list<std::string> &params) {
  Description *descriptor = nullptr;
  if(base_name == "MetadataMSI")           descriptor = new TypeMetadataMSI(type_name);
  if(base_name == "Data64B")               descriptor = new TypeData64B(type_name);
  if(base_name == "CacheArrayNorm")        descriptor = new TypeCacheArrayNorm(type_name);
  if(base_name == "CacheSkewed")           descriptor = new TypeCacheSkewed(type_name);
  if(base_name == "CacheNorm")             descriptor = new TypeCacheNorm(type_name);
  if(base_name == "OuterPortMSIUncached")  descriptor = new TypeOuterPortMSIUncached(type_name);
  if(base_name == "OuterPortMSI")          descriptor = new TypeOuterPortMSI(type_name);
  if(base_name == "InnerPortMSIUncached")  descriptor = new TypeInnerPortMSIUncached(type_name);
  if(base_name == "InnerPortMSIBroadcast") descriptor = new TypeInnerPortMSIBroadcast(type_name);
  if(base_name == "CoreInterfaceMSI")      descriptor = new TypeCoreInterfaceMSI(type_name);
  if(base_name == "CoherentCacheNorm")     descriptor = new TypeCoherentCacheNorm(type_name);
  if(base_name == "CoherentL1CacheNorm")   descriptor = new TypeCoherentL1CacheNorm(type_name);
  if(base_name == "IndexNorm")             descriptor = new TypeIndexNorm(type_name);
  if(base_name == "IndexSkewed")           descriptor = new TypeIndexSkewed(type_name);
  if(base_name == "IndexRandom")           descriptor = new TypeIndexRandom(type_name);
  if(base_name == "ReplaceFIFO")           descriptor = new TypeReplaceFIFO(type_name);
  if(base_name == "ReplaceLRU")            descriptor = new TypeReplaceLRU(type_name);

  if(nullptr == descriptor) {
    std::cerr << "[Decode] Fail to match `" << base_name << "' with a known base type." << std::endl; 
    return false;
  }
  if(!descriptor->set(params)) return false;
  add(type_name, descriptor);
  descriptor->emit_header();
  codegendb.type_declarations.push_back(descriptor);
  return true;
}

DescriptionDB typedb;

bool Description::parse_int(const std::string &param, int &rv) const {
  if(typedb.consts.count(param)) {
    rv = typedb.consts[param];
    return true;
  }

  try { rv = std::stoi(param); }
  catch(std::invalid_argument &e) {
    std::cerr << "[Integer] Fail to parse `" << e.what() << "' into an integer when defining a new type from `" << name << "'." << std::endl;
    return false;
  }

  return true;
}

bool Description::parse_bool(const std::string &param, bool &rv) const {
  if(param == "true"  || param == "TRUE" || param == "T") { rv = true; return true; }
  if(param == "false" || param == "FALSE" || param == "F") { rv = false; return true; }
  
  if(typedb.consts.count(param)) {
    rv = typedb.consts[param];
    return true;
  }

  try { rv = std::stoi(param); }
  catch(std::invalid_argument &e) {
    std::cerr << "[Integer] Fail to parse `" << e.what() << "' into boolean when defining a new type from `" << name << "'." << std::endl;
    return false;
  }

  return true;
}

void Description::emit_header() { codegendb.add_header("cache/cache.hpp"); }
void TypeMetadataMSI::emit_header() { codegendb.add_header("cache/msi.hpp"); }
void TypeOuterPortMSIUncached::emit_header() { codegendb.add_header("cache/msi.hpp"); }
void TypeOuterPortMSI::emit_header() { codegendb.add_header("cache/msi.hpp"); }
void TypeInnerPortMSIUncached::emit_header() { codegendb.add_header("cache/msi.hpp"); }
void TypeInnerPortMSIBroadcast::emit_header() { codegendb.add_header("cache/msi.hpp"); }
void TypeCoreInterfaceMSI::emit_header() { codegendb.add_header("cache/msi.hpp"); }
void TypeCoherentCacheBase::emit_header() { codegendb.add_header("cache/coherence.hpp"); }
void TypeIndexFuncBase::emit_header() { codegendb.add_header("cache/index.hpp"); }
void TypeReplaceFuncBase::emit_header() { codegendb.add_header("cache/replace.hpp"); }
