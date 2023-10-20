#ifndef CM_DSL_TYPE_DESCRIPTION_HPP
#define CM_DSL_TYPE_DESCRIPTION_HPP

#include <iostream>
#include <fstream>
#include <set>
#include <map>
#include <list>
#include <string>
#include <cstdlib>
#include <cassert>

////////////////////////////// A gloabl database ///////////////////////////////////////////////

struct Description;

struct DescriptionDB
{
  std::map<std::string, Description *> types;
  ~DescriptionDB();

  DescriptionDB();
  bool create(const std::string &type_name, const std::string &base_name, std::list<std::string> &params);

  bool add(const std::string &name, Description *d) {
    if(types.count(name)) {
      std::cerr << "[Double Definition] Type description: `" << name << "' has already been defined!" << std::endl;
      return false;
    }
    types[name] = d;
    return true;
  }
};

extern DescriptionDB typedb;

// base classes for all description
struct Description
{
  const std::string name; // name of the description
  const std::string tname;

  Description(const std::string &name, const std::string &tname): name(name), tname(tname){}

  virtual bool set(std::list<std::string> &values) { return true; }
  virtual void emit(std::ofstream &file) { file << "typedef " << tname << " " << this->name << ";" << std::endl; }
  virtual void emit_header();
  virtual void emit_declaration(std::ofstream &file, bool hpp,
                                const std::string &ename, const std::list<std::string> &params,  unsigned int size) {}
  virtual void emit_initialization(std::ofstream &file,
                                   const std::string &ename, const std::list<std::string> &params,  unsigned int size) {}
  virtual std::string get_outer() { return std::string(); }
  virtual std::string get_inner() { return std::string(); }

};

struct TypeVoid : public Description {
  TypeVoid(const std::string &name) : Description(name, "void") {}
};


////////////////////////////// Cache Basic ///////////////////////////////////////////////

struct TypeData64B : public Description {
  TypeData64B(const std::string &name = "", const std::string &tname ="Data64B") : Description(name, tname) {}
};

struct TypeCacheArrayNorm : public Description {
  int IW, NW; std::string MT, DT;
  TypeCacheArrayNorm(const std::string &name, const std::string &tname ="CacheArrayNorm") : Description(name, tname) {}
  virtual bool set(std::list<std::string> &values);
  virtual void emit(std::ofstream &file);
};

struct TypeCacheSkewed : public Description {
  int IW, NW, P; std::string MT, DT, IDX, RPC, DLY; bool EnMon;
  TypeCacheSkewed(const std::string &name, const std::string &tname ="CacheSkewed") : Description(name, tname) {}
  virtual bool set(std::list<std::string> &values);
  virtual void emit(std::ofstream &file);
};

struct TypeCacheNorm : public Description {
  int IW, NW; std::string MT, DT, IDX, RPC, DLY; bool EnMon;
  TypeCacheNorm(const std::string &name, const std::string &tname ="CacheNorm") : Description(name, tname) {}
  virtual bool set(std::list<std::string> &values);
  virtual void emit(std::ofstream &file);
};

////////////////////////////// Coherent///////////////////////////////////////////////

struct TypeMSI : public Description {
  TypeMSI(const std::string &name, const std::string &tname) : Description(name, tname) {}
  virtual void emit_header();
};

struct TypeMSIPolicy : public TypeMSI {
  std::string MT; bool isL1, isLLC;
  TypeMSIPolicy(const std::string &name, const std::string &tname ="MSIPolicy") : TypeMSI(name, tname) {}
  virtual bool set(std::list<std::string> &values);
  virtual void emit(std::ofstream &file);
  virtual void emit_initialization(std::ofstream &file,
                                   const std::string &ename, const std::list<std::string> &params, unsigned int size) {
    assert(size == 1 && "policy should not be initialized into multiple.");
    file << "  " << name << " *" << ename << " = new " << name << "(";
    auto it = params.begin();
    while(it != params.end()) {
      file << *it;
      ++it;
      if(it != params.end()) file << ", ";
    }
    file <<");" << std::endl;
  }
};

struct TypeMetadataMSI : public TypeMSI {
  int AW, IW, TOfst;
  std::string ST;
  TypeMetadataMSI(const std::string &name, const std::string &tname ="MetadataMSI")
    : TypeMSI(name, tname),
      ST("MetadataMSIBrodcast") // default to broadcast
  {}
  virtual bool set(std::list<std::string> &values);
  virtual void emit(std::ofstream &file);
};

struct TypeCoherent : public Description {
  TypeCoherent(const std::string &name, const std::string &tname) : Description(name, tname) {}
  virtual void emit_header();
  virtual std::string get_outer() { return "->outer"; }
  virtual std::string get_inner() { return "->inner"; }
};

struct TypeOuterCohPortUncached : public TypeCoherent {
  TypeOuterCohPortUncached(const std::string &name, const std::string &tname ="OuterCohPortUncached") : TypeCoherent(name, tname) {}
};

struct TypeOuterCohPort : public TypeCoherent {
  TypeOuterCohPort(const std::string &name, const std::string &tname ="OuterCohPort") : TypeCoherent(name, tname) {}
};

struct TypeInnerCohPortUncached : public TypeCoherent {
  TypeInnerCohPortUncached(const std::string &name, const std::string &tname ="InnerCohPortUncached") : TypeCoherent(name, tname) {}
};

struct TypeInnerCohPort : public TypeCoherent {
  TypeInnerCohPort(const std::string &name, const std::string &tname ="InnerCohPort") : TypeCoherent(name, tname) {}
};

struct TypeCoherentCache : public TypeCoherent {
  TypeCoherentCache(const std::string &name, const std::string &tname) : TypeCoherent(name, tname) {}
  virtual void emit_declaration(std::ofstream &file, bool hpp,
                                const std::string &ename, const std::list<std::string> &params,  unsigned int size) {
    if(hpp) file << "extern ";
    file << "std::vector<" << name << " *> " << ename;
    if(hpp)  file << ";" << std::endl;
    else     file << "(" << size << ");" << std::endl;
  }
  virtual void emit_initialization(std::ofstream &file,
                                   const std::string &ename, const std::list<std::string> &params,  unsigned int size) {
    file << "  for(int i=0; i<" << size << "; i++) ";
    file << ename << "[i] = new " << name << "(";
    for(auto p: params) file << p << ", ";
    file << "std::string(\"" << ename << "\") + \"_\" + std::to_string(i)";
    file <<");" << std::endl;
  }
};

struct TypeCoherentCacheNorm : public TypeCoherentCache {
  std::string CacheT, OuterT, InnerT;
  TypeCoherentCacheNorm(const std::string &name, const std::string &tname ="CoherentCacheNorm")
    : TypeCoherentCache(name, tname),
      OuterT("OuterCohPort"), InnerT("InnerCohPort") // default to full functional ports
  {}
  virtual bool set(std::list<std::string> &values);  
  virtual void emit(std::ofstream &file);
  
};

struct TypeCoherentL1CacheNorm : public TypeCoherentCache {
  std::string CacheT, OuterT, CoreT;
  TypeCoherentL1CacheNorm(const std::string &name, const std::string &tname ="CoherentL1CacheNorm")
    : TypeCoherentCache(name, tname),
      OuterT("OuterCohPort"), CoreT("CoreInterface") // default to full functional output port and core interface
  {}
  virtual bool set(std::list<std::string> &values);  
  virtual void emit(std::ofstream &file);
};

struct TypeSliceDispatcher : public TypeCoherentCache {
  std::string HT;
  TypeSliceDispatcher(const std::string &name, const std::string &tname ="SliceDispatcher") : TypeCoherentCache(name, tname) {}
  virtual bool set(std::list<std::string> &values);
  virtual void emit(std::ofstream &file);
};

////////////////////////////// Memory ///////////////////////////////////////////////

struct TypeSimpleMemoryModel : public TypeCoherentCache {
  std::string DT, DLY;
  TypeSimpleMemoryModel(const std::string &name, const std::string &tname ="SimpleMemoryModel") : TypeCoherentCache(name, tname) {}
  virtual bool set(std::list<std::string> &values);
  virtual void emit(std::ofstream &file);
  virtual void emit_header();
};

////////////////////////////// Index ///////////////////////////////////////////////

struct TypeIndex : public Description {
  int IW, IOfst;
  TypeIndex(const std::string &name, const std::string &tname) : Description(name, tname) {}
  virtual bool set(std::list<std::string> &values);  
  virtual void emit(std::ofstream &file);
  virtual void emit_header();
};

struct TypeIndexNorm : public TypeIndex {
  TypeIndexNorm(const std::string &name, const std::string &tname ="IndexNorm") : TypeIndex(name, tname) {}
};

struct TypeIndexRandom : public TypeIndex {
  TypeIndexRandom(const std::string &name, const std::string &tname ="IndexRandom") : TypeIndex(name, tname) {}
};

struct TypeIndexSkewed : public TypeIndex {
  int P;
  TypeIndexSkewed(const std::string &name, const std::string &tname ="IndexSkewed") : TypeIndex(name, tname) {}
  virtual bool set(std::list<std::string> &values);
  virtual void emit(std::ofstream &file);
};

////////////////////////////// Replacer ///////////////////////////////////////////////

struct TypeReplace : public Description {
  int IW, NW;
  TypeReplace(const std::string &name, const std::string &tname) : Description(name, tname) {}
  virtual bool set(std::list<std::string> &values);  
  virtual void emit(std::ofstream &file);
  virtual void emit_header();
};

struct TypeReplaceFIFO : public TypeReplace {
  TypeReplaceFIFO(const std::string &name, const std::string &tname ="ReplaceFIFO") : TypeReplace(name, tname) {}
};

struct TypeReplaceLRU : public TypeReplace {
  TypeReplaceLRU(const std::string &name, const std::string &tname ="ReplaceLRU") : TypeReplace(name, tname) {}
};

struct TypeReplaceRandom : public TypeReplace {
  TypeReplaceRandom(const std::string &name, const std::string &tname ="ReplaceRandom") : TypeReplace(name, tname) {}
};

struct TypeReplaceCompleteRandom : public TypeReplace {
  TypeReplaceCompleteRandom(const std::string &name, const std::string &tname ="ReplaceCompleteRandom") : TypeReplace(name, tname) {}
};

////////////////////////////// hasher ///////////////////////////////////////////////

struct TypeSliceHash : public Description {
  TypeSliceHash(const std::string &name, const std::string &tname) : Description(name, tname) {}
  virtual void emit_header();
};

struct TypeSliceHashNorm : public TypeSliceHash {
  int NLLC, BlkOfst;
  TypeSliceHashNorm(const std::string &name, const std::string &tname ="SliceHashNorm") : TypeSliceHash(name, tname) {}
  virtual bool set(std::list<std::string> &values);
  virtual void emit(std::ofstream &file);
};

struct TypeSliceHashIntelCAS : public TypeSliceHash {
  int NLLC;
  TypeSliceHashIntelCAS(const std::string &name, const std::string &tname ="SliceHashIntelCAS") : TypeSliceHash(name, tname) {}
  virtual bool set(std::list<std::string> &values);
  virtual void emit(std::ofstream &file);
};

////////////////////////////// Delay ///////////////////////////////////////////////

struct TypeDelay : public Description {
  TypeDelay(const std::string &name, const std::string &tname) : Description(name, tname) {}
  virtual void emit_header();
};

struct TypeDelayL1 : public TypeDelay {
  int dhit, dreplay, dtran;
  TypeDelayL1(const std::string &name, const std::string &tname ="DelayL1") : TypeDelay(name, tname) {}
  virtual bool set(std::list<std::string> &values);
  virtual void emit(std::ofstream &file);
};

struct TypeDelayCoherentCache : public TypeDelay {
  int dhit, dtranUp, dtranDown;
  TypeDelayCoherentCache(const std::string &name, const std::string &tname ="DelayCoherentCache") : TypeDelay(name, tname) {}
  virtual bool set(std::list<std::string> &values);
  virtual void emit(std::ofstream &file);
};

struct TypeDelayMemory : public TypeDelay {
  int dtran;
  TypeDelayMemory(const std::string &name, const std::string &tname ="DelayMemory") : TypeDelay(name, tname) {}
  virtual bool set(std::list<std::string> &values);
  virtual void emit(std::ofstream &file);
};

////////////////////////////// MIRAGE Types ///////////////////////////////////////////////

struct TypeMirage : public Description {
  TypeMirage(const std::string &name, const std::string &tname) : Description(name, tname) {}
  virtual void emit_header();
};

struct TypeMirageMetadataMSI : public TypeMetadataMSI {
  TypeMirageMetadataMSI(const std::string &name, const std::string &tname ="MirageMetadataMSI") : TypeMetadataMSI(name, tname) {}
  virtual void emit_header();
};

struct TypeMirageDataMeta : public TypeMirage {
  TypeMirageDataMeta(const std::string &name, const std::string &tname ="MirageDataMeta") : TypeMirage(name, tname) {}
};


struct TypeCacheMirage : public  TypeMirage {
  int IW, NW, EW, P, RW; std::string MT, DT, MTDT, MIDX, DIDX, MRPC, DRPC, DLY; bool EnMon, EnableRelocation;
  TypeCacheMirage(const std::string &name, const std::string &tname ="CacheMirage") : TypeMirage(name, tname) {}
  virtual bool set(std::list<std::string> &values);
  virtual void emit(std::ofstream &file);
};

struct TypeMirageMSIPolicy : public TypeMirage {
  TypeMirageMSIPolicy(const std::string &name, const std::string &tname ="MirageMSIPolicy") : TypeMirage(name, tname) {}
};

#endif
