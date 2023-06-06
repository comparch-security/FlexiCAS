#ifndef CM_DSL_TYPE_DESCRIPTION_HPP
#define CM_DSL_TYPE_DESCRIPTION_HPP

#include <iostream>
#include <fstream>
#include <set>
#include <map>
#include <list>
#include <string>
#include <cstdlib>

////////////////////////////// A gloabl database ///////////////////////////////////////////////

class Description;

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
class Description
{
public:
  const std::string name; // name of the description
  std::set<std::string> types; // list the types matching this description

  bool check(const std::string& tname, const std::string& param, const std::string& value, const std::string& constraint, bool allow_void) {
    if(allow_void) {
      if (value == "void") return true;
      if (typedb.types.count(value) && typedb.types[value]->comply("void")) return true;
    }
    if(!typedb.types.count(value)) {
      std::cerr << "[Undefined Type] " << tname << "'" << param << ": `" << value << "' is not defined!" << std::endl;
      return false;
    }
    if(!typedb.types[value]->comply(constraint)) {
      std::cerr << "[Constraint] " << tname << "'" << param << ": `" << value << "' must be a derived type of " << constraint << "!" << std::endl;
      return false;
    }
    return true;
  }

  Description(const std::string &name): name(name) {}

  virtual bool set(std::list<std::string> &values) = 0;
  // check type compliant
  bool comply(const std::string& c) { return types.count(c); }
  virtual void emit(std::ofstream &file) = 0;
  virtual void emit_header(); // add the header files required fro this type
  virtual std::string get_outer() { return std::string(); }
  virtual std::string get_inner() { return std::string(); }

};

////////////////////////////// Data Types ///////////////////////////////////////////////

class TypeVoid : public Description {
  const std::string tname;
public: TypeVoid(const std::string &name) : Description(name), tname("void") { types.insert("void"); }
  virtual bool set(std::list<std::string> &values);
  virtual void emit(std::ofstream &file);
};


////////////////////////////// Data Types ///////////////////////////////////////////////

class TypeCMMetadataBase : public Description {
public: TypeCMMetadataBase(const std::string &name) : Description(name) { types.insert("CMMetadataBase"); }
};

class TypeMetadataMSIBase : public TypeCMMetadataBase {
public: TypeMetadataMSIBase(const std::string &name) : TypeCMMetadataBase(name) { types.insert("MetadataMSIBase"); }
};

class TypeMetadataMSI : public TypeMetadataMSIBase {
  int AW, IW, TOfst;
  const std::string tname;
public:
  TypeMetadataMSI(const std::string &name) : TypeMetadataMSIBase(name), tname("MetadataMSI") {}
  virtual bool set(std::list<std::string> &values);
  virtual void emit(std::ofstream &file);
  virtual void emit_header();
};

class TypeCMDataBase : public Description {
public: TypeCMDataBase(const std::string &name) : Description(name) { types.insert("CMDataBase"); }
};

class TypeData64B : public TypeCMDataBase
{
  const std::string tname;
public:
  TypeData64B(const std::string &name) : TypeCMDataBase(name), tname("Data64B") { types.insert("Data64B"); }
  TypeData64B() : TypeCMDataBase(""), tname("Data64B") { types.insert("Data64B"); }
  virtual bool set(std::list<std::string> &values);
  virtual void emit(std::ofstream &file);
};

////////////////////////////// Cache Array ///////////////////////////////////////////////

class TypeCacheArrayBase : public Description {
public: TypeCacheArrayBase(const std::string &name) : Description(name) { types.insert("CacheArrayBase"); }
};

class TypeCacheArrayNorm : public TypeCacheArrayBase
{
  int IW, NW; std::string MT, DT;
  const std::string tname;
public:
  TypeCacheArrayNorm(const std::string &name) : TypeCacheArrayBase(name), tname("CacheArrayNorm") {}
  virtual bool set(std::list<std::string> &values);
  virtual void emit(std::ofstream &file);
};

////////////////////////////// Cache ///////////////////////////////////////////////

class TypeCacheBase : public Description {
public: TypeCacheBase(const std::string &name) : Description(name) { types.insert("CacheBase"); }
};

class TypeCacheSkewed : public TypeCacheBase
{
  int IW, NW, P; std::string MT, DT, IDX, RPC, DLY; bool EnMon;
  const std::string tname;
public:
  TypeCacheSkewed(const std::string &name) : TypeCacheBase(name), tname("CacheSkewed") {}
  virtual bool set(std::list<std::string> &values); 
  virtual void emit(std::ofstream &file);
};

class TypeCacheNorm : public TypeCacheBase
{
  int IW, NW; std::string MT, DT, IDX, RPC, DLY; bool EnMon;
  const std::string tname;
public:
  TypeCacheNorm(const std::string &name) : TypeCacheBase(name), tname("CacheNorm") {}
  virtual bool set(std::list<std::string> &values); 
  virtual void emit(std::ofstream &file);
};

////////////////////////////// Coherent Cache ///////////////////////////////////////////////

class TypeOuterCohPortBase : public Description {
public: TypeOuterCohPortBase(const std::string &name) : Description(name) { types.insert("OuterCohPortBase"); types.insert("CohClientBase"); }
};

class TypeOuterPortMSIUncached : public TypeOuterCohPortBase {
  std::string MT, DT;
  const std::string tname;
public:
  TypeOuterPortMSIUncached(const std::string &name) : TypeOuterCohPortBase(name), tname("OuterPortMSIUncached") {}
  virtual bool set(std::list<std::string> &values);
  virtual void emit(std::ofstream &file);
  virtual void emit_header();
};

class TypeOuterPortMSI : public TypeOuterCohPortBase {
  std::string MT, DT;
  const std::string tname;
public:
  TypeOuterPortMSI(const std::string &name) : TypeOuterCohPortBase(name), tname("OuterPortMSI") {}
  virtual bool set(std::list<std::string> &values);  
  virtual void emit(std::ofstream &file);
  virtual void emit_header();
};

class TypeInnerCohPortBase : public Description {
public: TypeInnerCohPortBase(const std::string &name) : Description(name) { types.insert("InnerCohPortBase"); types.insert("CohMasterBase"); }
};

class TypeInnerPortMSIUncached : public TypeInnerCohPortBase
{
  std::string MT, DT; bool isLLC;
  const std::string tname;
public:
  TypeInnerPortMSIUncached(const std::string &name) : TypeInnerCohPortBase(name), tname("InnerPortMSIUncached") {}
  virtual bool set(std::list<std::string> &values);  
  virtual void emit(std::ofstream &file);
  virtual void emit_header();
};

class TypeInnerPortMSIBroadcast : public TypeInnerCohPortBase
{
  std::string MT, DT; bool isLLC;
  const std::string tname;
public:
  TypeInnerPortMSIBroadcast(const std::string &name) : TypeInnerCohPortBase(name), tname("InnerPortMSIBroadcast") {}
  virtual bool set(std::list<std::string> &values);  
  virtual void emit(std::ofstream &file);
  virtual void emit_header();
};

class TypeCoreInterfaceBase : public TypeInnerCohPortBase {
public: TypeCoreInterfaceBase(const std::string &name) : TypeInnerCohPortBase(name) { types.insert("CoreInterfaceBase"); }
};

class TypeCoreInterfaceMSI : public TypeCoreInterfaceBase
{
  std::string MT, DT; bool enableDelay, isLLC;
  const std::string tname;
public:
  TypeCoreInterfaceMSI(const std::string &name) : TypeCoreInterfaceBase(name), tname("CoreInterfaceMSI") {}
  virtual bool set(std::list<std::string> &values);  
  virtual void emit(std::ofstream &file);
  virtual void emit_header();
};

class TypeCoherentCacheBase : public Description {
public:
  TypeCoherentCacheBase(const std::string &name) : Description(name) { types.insert("CoherentCacheBase"); }

  virtual void emit_header();
};

class TypeCoherentCacheNorm : public TypeCoherentCacheBase
{
  std::string CacheT, OuterT, InnerT;
  const std::string tname;
public:
  TypeCoherentCacheNorm(const std::string &name) : TypeCoherentCacheBase(name), tname("CoherentCacheNorm") {}
  virtual bool set(std::list<std::string> &values);  
  virtual void emit(std::ofstream &file);
  virtual std::string get_outer() { return "->outer"; }
  virtual std::string get_inner() { return "->inner"; }
};

class TypeCoherentL1CacheNorm : public TypeCoherentCacheBase
{
  std::string CacheT, OuterT, CoreT; bool isLLC;
  const std::string tname;
public:
  TypeCoherentL1CacheNorm(const std::string &name) : TypeCoherentCacheBase(name), tname("CoherentL1CacheNorm") {}
  virtual bool set(std::list<std::string> &values);  
  virtual void emit(std::ofstream &file);
  virtual std::string get_outer() { return "->outer"; }
  virtual std::string get_inner() { return "->inner"; }
};

////////////////////////////// Memory ///////////////////////////////////////////////

class TypeSimpleMemoryModel : public TypeInnerCohPortBase
{
  std::string DT, DLY;
  const std::string tname;
public:
  TypeSimpleMemoryModel(const std::string &name) : TypeInnerCohPortBase(name), tname("SimpleMemoryModel") {}
  virtual bool set(std::list<std::string> &values);
  virtual void emit(std::ofstream &file);
  virtual void emit_header();
};

////////////////////////////// Index ///////////////////////////////////////////////

class TypeIndexFuncBase : public Description {
public:
  TypeIndexFuncBase(const std::string &name) : Description(name) { types.insert("IndexFuncBase"); }
  virtual void emit_header();
};

class TypeIndexNorm : public TypeIndexFuncBase
{
  int IW, IOfst;
  const std::string tname;
public:
  TypeIndexNorm(const std::string &name) : TypeIndexFuncBase(name), tname("IndexNorm") {}
  virtual bool set(std::list<std::string> &values);  
  virtual void emit(std::ofstream &file);
};

class TypeIndexSkewed : public TypeIndexFuncBase
{
  int IW, IOfst, P;
  const std::string tname;
public:
  TypeIndexSkewed(const std::string &name) : TypeIndexFuncBase(name), tname("IndexSkewed") {}
  virtual bool set(std::list<std::string> &values);
  virtual void emit(std::ofstream &file);
};

class TypeIndexRandom : public TypeIndexFuncBase
{
  int IW, IOfst;
  const std::string tname;
public:
  TypeIndexRandom(const std::string &name) : TypeIndexFuncBase(name), tname("IndexRandom") {}
  virtual bool set(std::list<std::string> &values);  
  virtual void emit(std::ofstream &file);
};

////////////////////////////// Replacer ///////////////////////////////////////////////

class TypeReplaceFuncBase : public Description {
public:
  TypeReplaceFuncBase(const std::string &name) : Description(name) { types.insert("ReplaceFuncBase"); }

  virtual void emit_header();
};

class TypeReplaceFIFO : public TypeReplaceFuncBase
{
  int IW, NW;
  const std::string tname;
public:
  TypeReplaceFIFO(const std::string &name) : TypeReplaceFuncBase(name), tname("ReplaceFIFO") {}
  virtual bool set(std::list<std::string> &values);  
  virtual void emit(std::ofstream &file);
};

class TypeReplaceLRU : public TypeReplaceFuncBase
{
  int IW, NW;
  const std::string tname;
public:
  TypeReplaceLRU(const std::string &name) : TypeReplaceFuncBase(name), tname("ReplaceLRU") {}
  virtual bool set(std::list<std::string> &values);
  virtual void emit(std::ofstream &file);
};

////////////////////////////// hasher and dispatcher ///////////////////////////////////////////////

class TypeSliceHashBase : public Description {
public:
  TypeSliceHashBase(const std::string &name) : Description(name) { types.insert("SliceHashBase"); }
  virtual void emit_header();
};

class TypeSliceHashNorm : public TypeSliceHashBase {
  int NLLC, BlkOfst;
  const std::string tname;
public:
  TypeSliceHashNorm(const std::string &name) : TypeSliceHashBase(name), tname("SliceHashNorm") {}
  virtual bool set(std::list<std::string> &values);
  virtual void emit(std::ofstream &file);
};

class TypeSliceHashIntelCAS : public TypeSliceHashBase {
  int NLLC;
  const std::string tname;
public:
  TypeSliceHashIntelCAS(const std::string &name) : TypeSliceHashBase(name), tname("SliceHashIntelCAS") {}
  virtual bool set(std::list<std::string> &values);
  virtual void emit(std::ofstream &file);
};

class TypeSliceDispatcher : public TypeInnerCohPortBase
{
  std::string HT;
  const std::string tname;
public:
  TypeSliceDispatcher(const std::string &name) : TypeInnerCohPortBase(name), tname("SliceDispatcher") {}
  virtual bool set(std::list<std::string> &values);
  virtual void emit(std::ofstream &file);
  virtual void emit_header();
};

////////////////////////////// Delay ///////////////////////////////////////////////

class TypeDelayBase : public Description {
public:
  TypeDelayBase(const std::string &name) : Description(name) { types.insert("DelayBase"); }
  virtual void emit_header();
};

class TypeDelayL1 : public TypeDelayBase
{
  int dhit, dreplay, dtran;
  const std::string tname;
public:
  TypeDelayL1(const std::string &name) : TypeDelayBase(name), tname("DelayL1") {}
  virtual bool set(std::list<std::string> &values);
  virtual void emit(std::ofstream &file);
};

class TypeDelayCoherentCache : public TypeDelayBase
{
  int dhit, dtranUp, dtranDown;
  const std::string tname;
public:
  TypeDelayCoherentCache(const std::string &name) : TypeDelayBase(name), tname("DelayCoherentCache") {}
  virtual bool set(std::list<std::string> &values);
  virtual void emit(std::ofstream &file);
};

class TypeDelayMemory : public TypeDelayBase
{
  int dtran;
  const std::string tname;
public:
  TypeDelayMemory(const std::string &name) : TypeDelayBase(name), tname("DelayMemory") {}
  virtual bool set(std::list<std::string> &values);
  virtual void emit(std::ofstream &file);
};

#endif
