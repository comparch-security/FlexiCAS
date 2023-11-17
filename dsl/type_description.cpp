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
  Description *descriptor = nullptr;
  if(base_name == "void")                        descriptor = new TypeVoid(type_name);
  if(base_name == "MetadataMSIBroadcast")        descriptor = new TypeMetadataMSIBroadcast(type_name);
  if(base_name == "MirageMetadataMSIBroadcast")  descriptor = new TypeMirageMetadataMSIBroadcast(type_name);
  if(base_name == "MirageDataMeta")              descriptor = new TypeMirageDataMeta(type_name);
  if(base_name == "Data64B")                     descriptor = new TypeData64B(type_name);
  if(base_name == "CacheArrayNorm")              descriptor = new TypeCacheArrayNorm(type_name);
  if(base_name == "CacheSkewed")                 descriptor = new TypeCacheSkewed(type_name);
  if(base_name == "CacheNorm")                   descriptor = new TypeCacheNorm(type_name);
  if(base_name == "MirageCache")                 descriptor = new TypeMirageCache(type_name);
  if(base_name == "MSIPolicy")                   descriptor = new TypeMSIPolicy(type_name);
  if(base_name == "MirageMSIPolicy")             descriptor = new TypeMirageMSIPolicy(type_name);
  if(base_name == "OuterCohPortUncached")        descriptor = new TypeOuterCohPortUncached(type_name);
  if(base_name == "OuterCohPort")                descriptor = new TypeOuterCohPort(type_name);
  if(base_name == "InnerCohPortUncached")        descriptor = new TypeInnerCohPortUncached(type_name);
  if(base_name == "InnerCohPort")                descriptor = new TypeInnerCohPort(type_name);
  if(base_name == "CoherentCacheNorm")           descriptor = new TypeCoherentCacheNorm(type_name);
  if(base_name == "CoherentL1CacheNorm")         descriptor = new TypeCoherentL1CacheNorm(type_name);
  if(base_name == "SimpleMemoryModel")           descriptor = new TypeSimpleMemoryModel(type_name);
  if(base_name == "IndexNorm")                   descriptor = new TypeIndexNorm(type_name);
  if(base_name == "IndexSkewed")                 descriptor = new TypeIndexSkewed(type_name);
  if(base_name == "IndexRandom")                 descriptor = new TypeIndexRandom(type_name);
  if(base_name == "ReplaceFIFO")                 descriptor = new TypeReplaceFIFO(type_name);
  if(base_name == "ReplaceLRU")                  descriptor = new TypeReplaceLRU(type_name);
  if(base_name == "ReplaceSRRIP")                descriptor = new TypeReplaceSRRIP(type_name);
  if(base_name == "ReplaceRandom")               descriptor = new TypeReplaceRandom(type_name);
  if(base_name == "SliceHashNorm")               descriptor = new TypeSliceHashNorm(type_name);
  if(base_name == "SliceHashIntelCAS")           descriptor = new TypeSliceHashIntelCAS(type_name);
  if(base_name == "SliceDispatcher")             descriptor = new TypeSliceDispatcher(type_name);
  if(base_name == "DelayL1")                     descriptor = new TypeDelayL1(type_name);
  if(base_name == "DelayCoherentCache")          descriptor = new TypeDelayCoherentCache(type_name);
  if(base_name == "DelayMemory")                 descriptor = new TypeDelayMemory(type_name);

  if(nullptr == descriptor) {
    std::cerr << "[Decode] Fail to match `" << base_name << "' with a known base type." << std::endl;
    return false;
  }
  if(!descriptor->set(params)) return false;
  if(!add(type_name, descriptor)) return false;
  descriptor->emit_header();
  codegendb.type_declarations.push_back(descriptor);
  return true;
}

void Description::emit_header() { codegendb.add_header("cache/cache.hpp"); }

#define PROGRESS_PAR(statement) if(it != values.end() && (statement)) ++it; else return false
#define PROGRESS_STR(statement) if(it != values.end()) { statement; ++it; } else return false
#define EARLY_TERM() if(it == values.end()) return true;

////////////////////////////// Cache Basic ///////////////////////////////////////////////

bool TypeCacheArrayNorm::set(std::list<std::string> &values) {
  auto it = values.begin();
  PROGRESS_PAR(codegendb.parse_int(*it, IW));
  PROGRESS_PAR(codegendb.parse_int(*it, NW));
  PROGRESS_STR(MT = *it);
  PROGRESS_STR(DT = *it);
  return true;
}

void TypeCacheArrayNorm::emit(std::ofstream &file) {
  file << "typedef " << tname << "<" << IW << "," << NW << "," << MT << "," << DT << "> " << this->name << ";" << std::endl;
}

bool TypeCacheSkewed::set(std::list<std::string> &values) {
  auto it = values.begin();
  PROGRESS_PAR(codegendb.parse_int(*it, IW));
  PROGRESS_PAR(codegendb.parse_int(*it, NW));
  PROGRESS_PAR(codegendb.parse_int(*it, P));
  PROGRESS_STR(MT  = *it);
  PROGRESS_STR(DT  = *it);
  PROGRESS_STR(IDX = *it);
  PROGRESS_STR(RPC = *it);
  PROGRESS_STR(DLY = *it);
  PROGRESS_PAR(codegendb.parse_bool(*it, EnMon));
  return true;
}
 
void TypeCacheSkewed::emit(std::ofstream &file) {
  file << "typedef " << tname << "<" << IW << "," << NW << "," << P << "," << MT << "," << DT << "," << IDX << "," << RPC << "," << DLY << "," << EnMon << "> " << this->name << ";" << std::endl;
}

bool TypeCacheNorm::set(std::list<std::string> &values) {
  auto it = values.begin();
  PROGRESS_PAR(codegendb.parse_int(*it, IW));
  PROGRESS_PAR(codegendb.parse_int(*it, NW));
  PROGRESS_STR(MT  = *it);
  PROGRESS_STR(DT  = *it);
  PROGRESS_STR(IDX = *it);
  PROGRESS_STR(RPC = *it);
  PROGRESS_STR(DLY = *it);
  PROGRESS_PAR(codegendb.parse_bool(*it, EnMon));
  return true;
}
 
void TypeCacheNorm::emit(std::ofstream &file) {
  file << "typedef " << tname << "<" << IW << "," << NW << "," << MT << "," << DT << "," << IDX << "," << RPC << "," << DLY << "," << EnMon << "> " << this->name << ";" << std::endl;
}

////////////////////////////// Coherent///////////////////////////////////////////////

void TypeMSI::emit_header() { codegendb.add_header("cache/mirage.hpp"); }

bool TypeMSIPolicy::set(std::list<std::string> &values) {
  auto it = values.begin();
  PROGRESS_STR(MT     = *it);
  PROGRESS_PAR(codegendb.parse_bool(*it, isL1));
  PROGRESS_PAR(codegendb.parse_bool(*it, isLLC));
  return true;
}

void TypeMSIPolicy::emit(std::ofstream &file) {
  file << "typedef " << tname << "<" << MT << "," << isL1 << "," << isLLC << "> " << this->name << ";" << std::endl;
}

bool TypeMetadataMSIBroadcast::set(std::list<std::string> &values) {
  auto it = values.begin();
  PROGRESS_PAR(codegendb.parse_int(*it, AW));
  PROGRESS_PAR(codegendb.parse_int(*it, IW));
  PROGRESS_PAR(codegendb.parse_int(*it, TOfst));
  return true;
}

void TypeMetadataMSIBroadcast::emit(std::ofstream &file) {
  file << "typedef " << tname << "<" << AW << "," << IW << "," << TOfst << "> " << this->name << ";" << std::endl;
}

void TypeCoherent::emit_header() { codegendb.add_header("cache/coherence.hpp"); }

bool TypeCoherentCacheNorm::set(std::list<std::string> &values) {
  auto it = values.begin();
  PROGRESS_STR(CacheT = *it); EARLY_TERM();
  PROGRESS_STR(OuterT = *it); EARLY_TERM();
  PROGRESS_STR(InnerT = *it);
  return true;
}
  
void TypeCoherentCacheNorm::emit(std::ofstream &file) {
  file << "typedef " << tname << "<" << CacheT << "," << OuterT << "," << InnerT << "> " << this->name << ";" << std::endl;
}

bool TypeCoherentL1CacheNorm::set(std::list<std::string> &values) {
  auto it = values.begin();
  PROGRESS_STR(CacheT = *it); EARLY_TERM();
  PROGRESS_STR(OuterT = *it); EARLY_TERM();
  PROGRESS_STR(CoreT = *it);
  return true;
}
  
void TypeCoherentL1CacheNorm::emit(std::ofstream &file) {
  file << "typedef " << tname << "<" << CacheT << "," << OuterT << "," << CoreT << "> " << this->name << ";" << std::endl;
}

bool TypeSliceDispatcher::set(std::list<std::string> &values) {
  auto it = values.begin();
  PROGRESS_STR(HT = *it);
  return true;
}

void TypeSliceDispatcher::emit(std::ofstream &file) {
  file << "typedef " << tname << "<" << HT << "> " << this->name << ";" << std::endl;
}

////////////////////////////// Memory ///////////////////////////////////////////////

void TypeSimpleMemoryModel::emit_header() { codegendb.add_header("cache/memory.hpp"); }

bool TypeSimpleMemoryModel::set(std::list<std::string> &values) {
  auto it = values.begin();
  PROGRESS_STR(DT  = *it);
  PROGRESS_STR(DLY = *it);
  return true;
}

void TypeSimpleMemoryModel::emit(std::ofstream &file) {
  file << "typedef " << tname << "<" << DT << "," << DLY << "> " << this->name << ";" << std::endl;
}

////////////////////////////// Index ///////////////////////////////////////////////

void TypeIndex::emit_header() { codegendb.add_header("cache/index.hpp"); }

bool TypeIndex::set(std::list<std::string> &values) {
  auto it = values.begin();
  PROGRESS_PAR(codegendb.parse_int(*it, IW));
  PROGRESS_PAR(codegendb.parse_int(*it, IOfst));
  return true;
}
  
void TypeIndex::emit(std::ofstream &file) {
  file << "typedef " << tname << "<" << IW << "," << IOfst << "> " << this->name << ";" << std::endl;
}  

bool TypeIndexSkewed::set(std::list<std::string> &values) {
  auto it = values.begin();
  PROGRESS_PAR(codegendb.parse_int(*it, IW));
  PROGRESS_PAR(codegendb.parse_int(*it, IOfst));
  PROGRESS_PAR(codegendb.parse_int(*it, P));
  return true;
}
  
void TypeIndexSkewed::emit(std::ofstream &file) {
  file << "typedef " << tname << "<" << IW << "," << IOfst << "," << P << "> " << this->name << ";" << std::endl;
}  

////////////////////////////// Replacer ///////////////////////////////////////////////

void TypeReplace::emit_header() { codegendb.add_header("cache/replace.hpp"); }

bool TypeReplace::set(std::list<std::string> &values) {
  auto it = values.begin();
  PROGRESS_PAR(codegendb.parse_int(*it, IW));
  PROGRESS_PAR(codegendb.parse_int(*it, NW)); EARLY_TERM();
  PROGRESS_PAR(codegendb.parse_bool(*it, EF));
  return true;
}
  
void TypeReplace::emit(std::ofstream &file) {
  file << "typedef " << tname << "<" << IW << "," << NW << "," << EF << "> " << this->name << ";" << std::endl;
}  


////////////////////////////// hasher ///////////////////////////////////////////////

void TypeSliceHash::emit_header() { codegendb.add_header("cache/slicehash.hpp"); }

bool TypeSliceHashNorm::set(std::list<std::string> &values) {
  auto it = values.begin();
  PROGRESS_PAR(codegendb.parse_int(*it, NLLC));
  PROGRESS_PAR(codegendb.parse_int(*it, BlkOfst));
  return true;
}

void TypeSliceHashNorm::emit(std::ofstream &file) {
  file << "typedef " << tname << "<" << NLLC << "," << BlkOfst << "> " << this->name << ";" << std::endl;
}

bool TypeSliceHashIntelCAS::set(std::list<std::string> &values) {
  auto it = values.begin();
  PROGRESS_PAR(codegendb.parse_int(*it, NLLC));
  return true;
}

void TypeSliceHashIntelCAS::emit(std::ofstream &file) {
  file << "typedef " << tname << "<" << NLLC << "> " << this->name << ";" << std::endl;
}

////////////////////////////// Delay ///////////////////////////////////////////////

void TypeDelay::emit_header() { codegendb.add_header("util/delay.hpp"); }

bool TypeDelayL1::set(std::list<std::string> &values) {
  auto it = values.begin();
  PROGRESS_PAR(codegendb.parse_int(*it, dhit));
  PROGRESS_PAR(codegendb.parse_int(*it, dreplay));
  PROGRESS_PAR(codegendb.parse_int(*it, dtran));
  return true;
}

void TypeDelayL1::emit(std::ofstream &file) {
  file << "typedef " << tname << "<" << dhit << "," << dreplay << "," << dtran << "> " << this->name << ";" << std::endl;
}

bool TypeDelayCoherentCache::set(std::list<std::string> &values) {
  auto it = values.begin();
  PROGRESS_PAR(codegendb.parse_int(*it, dhit));
  PROGRESS_PAR(codegendb.parse_int(*it, dtranUp));
  PROGRESS_PAR(codegendb.parse_int(*it, dtranDown));
  return true;
}

void TypeDelayCoherentCache::emit(std::ofstream &file) {
  file << "typedef " << tname << "<" << dhit << "," << dtranUp << "," << dtranDown << "> " << this->name << ";" << std::endl;
}

bool TypeDelayMemory::set(std::list<std::string> &values) {
  auto it = values.begin();
  PROGRESS_PAR(codegendb.parse_int(*it, dtran));
  return true;
}

void TypeDelayMemory::emit(std::ofstream &file) {
  file << "typedef " << tname << "<" << dtran << "> " << this->name << ";" << std::endl;
}

////////////////////////////// MIRAGE Types ///////////////////////////////////////////////

void TypeMirage::emit_header() { codegendb.add_header("cache/mirage.hpp"); }

void TypeMirageMetadataMSIBroadcast::emit_header() { codegendb.add_header("cache/mirage.hpp"); }

void TypeMirageMetadataMSIBroadcast::emit(std::ofstream &file) {
  file << "typedef " << tname << "<" << AW << "," << IW << "," << TOfst << "> " << this->name << ";" << std::endl;
}

bool TypeMirageCache::set(std::list<std::string> &values) {
  auto it = values.begin();
  PROGRESS_PAR(codegendb.parse_int(*it, IW));
  PROGRESS_PAR(codegendb.parse_int(*it, NW));
  PROGRESS_PAR(codegendb.parse_int(*it, EW));
  PROGRESS_PAR(codegendb.parse_int(*it, P));
  PROGRESS_PAR(codegendb.parse_int(*it, RW));
  PROGRESS_STR(MT   = *it);
  PROGRESS_STR(DT   = *it);
  PROGRESS_STR(MTDT = *it);
  PROGRESS_STR(MIDX = *it);
  PROGRESS_STR(DIDX = *it);
  PROGRESS_STR(MRPC = *it);
  PROGRESS_STR(DRPC = *it);
  PROGRESS_STR(DLY  = *it);
  PROGRESS_PAR(codegendb.parse_bool(*it, EnMon));
  PROGRESS_PAR(codegendb.parse_bool(*it, EnableRelocation));
  return true;
}

void TypeMirageCache::emit(std::ofstream &file) {
  file << "typedef " << tname << "<" << IW << "," << NW << "," << EW << "," << P << "," << RW <<  "," << MT << "," << DT << "," << MTDT << "," << MIDX << "," << DIDX << "," << MRPC << "," << DRPC << "," << DLY << "," << EnMon << "," << EnableRelocation << "> " << this->name << ";" << std::endl;
}
