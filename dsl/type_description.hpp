#ifndef CM_DSL_TYPE_DESCRIPTION_HPP
#define CM_DSL_TYPE_DESCRIPTION_HPP

#include <iostream>
#include <fstream>
#include <set>
#include <map>
#include <list>
#include <string>
#include <cstdlib>

class Description;

struct DescriptionDB
{
  std::map<std::string, Description *> types;
  DescriptionDB();
  ~DescriptionDB();

  void add(const std::string &name, Description *d) {
    if(types.count(name)) {
      std::cerr << "[Double Definition] Type description: `" << name << "' has already been defined!" << std::endl;
      exit(-1);
    }
    types["name"] = d;
  }
};

extern DescriptionDB typedb;

// base classes for all description
class Description
{
protected:
  const std::string name; // name of the description
  std::set<std::string> types; // list the types matching this description

  bool check(const std::string& tname, const std::string& param, const std::string& value, const std::string& constraint, bool allow_void) {
    if(allow_void && value == "void") return true;
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
public:
  Description(const std::string &name): name(name) {}

  virtual bool set(std::list<std::string> &values) = 0;
  // check type compliant
  bool comply(const std::string& c) { return types.count(c); }
  // generate type secription
  virtual void emit(std::ofstream &file) = 0;
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
  
  virtual bool set(std::list<std::string> &values) {
    if(values.size() != 3) {
      std::cerr << "[Misnatching Paramater] " << tname << " needs 3 parameters!" << std::endl;
      return false;
    }
    auto it = values.begin();
    AW = std::stoi(*it); it++;
    IW = std::stoi(*it); it++;
    TOfst = std::stoi(*it); it++;
    return true;
  }

  virtual void emit(std::ofstream &file) {
    file << "typedef " << tname << "<" << AW << "," << IW << "," << TOfst << "> " << this->name << ";" << std::endl;
  }
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

  virtual bool set(std::list<std::string> &values) {
    std::cerr << "[No Paramater] " << tname << " supports no parameter!" << std::endl;
    return false;
  }

  virtual void emit(std::ofstream &file) {
    if(!this->name.empty())
      file << "typedef " << tname << " " << this->name << ";" << std::endl;
  }
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

  virtual bool set(std::list<std::string> &values) {
    if(values.size() != 4) {
      std::cerr << "[Misnatching Paramater] " << tname << " needs 4 parameters!" << std::endl;
      return false;
    }
    auto it = values.begin();
    IW = std::stoi(*it); it++;
    NW = std::stoi(*it); it++;
    MT = *it; if(this->check(tname, "MT", *it, "CMMetadataBase", false)) return false; it++;
    DT = *it; if(this->check(tname, "DT", *it, "CMDataBase", true)) return false; it++;
    return true;
  }

  virtual void emit(std::ofstream &file) {
    file << "typedef " << tname << "<" << IW << "," << NW << "," << MT << "," << DT << "> " << this->name << ";" << std::endl;
  }
};

////////////////////////////// Cache ///////////////////////////////////////////////

class TypeCacheBase : public Description {
public: TypeCacheBase(const std::string &name) : Description(name) { types.insert("CacheBase"); }
};

class TypeCacheSkewed : public TypeCacheBase
{
  int IW, NW, P; std::string MT, DT, IDX, RPC; bool EnMon;
  const std::string tname;
public:
  TypeCacheSkewed(const std::string &name) : TypeCacheBase(name), tname("CacheSkewed") {}

  virtual bool set(std::list<std::string> &values) {
    if(values.size() != 8) {
      std::cerr << "[Misnatching Paramater] " << tname << " needs 8 parameters!" << std::endl;
      return false;
    }
    auto it = values.begin();
    IW = std::stoi(*it); it++;
    NW = std::stoi(*it); it++;
    P = std::stoi(*it); it++;
    MT  = *it; if(this->check(tname, "MT", *it, "CMMetadataBase", false)) return false; it++;
    DT  = *it; if(this->check(tname, "DT", *it, "CMDataBase", true)) return false; it++;
    IDX = *it; if(this->check(tname, "IDX", *it, "IndexFuncBase", false)) return false; it++;
    RPC = *it; if(this->check(tname, "RPC", *it, "ReplaceFuncBase", false)) return false; it++;
    EnMon = std::stoi(*it); it++;
    return true;
  }
 
  virtual void emit(std::ofstream &file) {
    file << "typedef " << tname << "<" << IW << "," << NW << "," << P << "," << MT << "," << DT << "," << IDX << "," << RPC << "," << EnMon << "> " << this->name << ";" << std::endl;
  }
};

class TypeCacheNorm : public TypeCacheBase
{
  int IW, NW; std::string MT, DT, IDX, RPC; bool EnMon;
  const std::string tname;
public:
  TypeCacheNorm(const std::string &name) : TypeCacheBase(name), tname("CacheNorm") {}

  virtual bool set(std::list<std::string> &values) {
    if(values.size() != 7) {
      std::cerr << "[Misnatching Paramater] " << tname << " needs 7 parameters!" << std::endl;
      return false;
    }
    auto it = values.begin();
    IW = std::stoi(*it); it++;
    NW = std::stoi(*it); it++;
    MT  = *it; if(this->check(tname, "MT", *it, "CMMetadataBase", false)) return false; it++;
    DT  = *it; if(this->check(tname, "DT", *it, "CMDataBase", true)) return false; it++;
    IDX = *it; if(this->check(tname, "IDX", *it, "IndexFuncBase", false)) return false; it++;
    RPC = *it; if(this->check(tname, "RPC", *it, "ReplaceFuncBase", false)) return false; it++;
    EnMon = std::stoi(*it); it++;
    return true;
  }
 
  virtual void emit(std::ofstream &file) {
    file << "typedef " << tname << "<" << IW << "," << NW << "," << MT << "," << DT << "," << IDX << "," << RPC << "," << EnMon << "> " << this->name << ";" << std::endl;
  }
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

  virtual bool set(std::list<std::string> &values) {
    if(values.size() != 2) {
      std::cerr << "[Misnatching Paramater] " << tname << " needs 2 parameters!" << std::endl;
      return false;
    }
    auto it = values.begin();
    MT  = *it; if(this->check(tname, "MT", *it, "MetadataMSIBase", false)) return false; it++;
    DT  = *it; if(this->check(tname, "DT", *it, "CMDataBase", true)) return false; it++;
    return true;
  }
  
  virtual void emit(std::ofstream &file) {
    file << "typedef " << tname << "<" << MT << "," << DT << "> " << this->name << ";" << std::endl;
  }  
};

class TypeOuterPortMSI : public TypeOuterCohPortBase {
  std::string MT, DT;
  const std::string tname;
public:
  TypeOuterPortMSI(const std::string &name) : TypeOuterCohPortBase(name), tname("OuterPortMSI") {}

  virtual bool set(std::list<std::string> &values) {
    if(values.size() != 2) {
      std::cerr << "[Misnatching Paramater] " << tname << " needs 2 parameters!" << std::endl;
      return false;
    }
    auto it = values.begin();
    MT  = *it; if(this->check(tname, "MT", *it, "MetadataMSIBase", false)) return false; it++;
    DT  = *it; if(this->check(tname, "DT", *it, "CMDataBase", true)) return false; it++;
    return true;
  }
  
  virtual void emit(std::ofstream &file) {
    file << "typedef " << tname << "<" << MT << "," << DT << "> " << this->name << ";" << std::endl;
  }  
};

class TypeInnerCohPortBase : public Description {
public: TypeInnerCohPortBase(const std::string &name) : Description(name) { types.insert("InnerCohPortBase"); types.insert("CohMasterBase"); }
};

class TypeInnerPortMSIUncached : public TypeInnerCohPortBase
{
  std::string MT, DT;
  const std::string tname;
public:
  TypeInnerPortMSIUncached(const std::string &name) : TypeInnerCohPortBase(name), tname("InnerPortMSIUncached") {}

  virtual bool set(std::list<std::string> &values) {
    if(values.size() != 2) {
      std::cerr << "[Misnatching Paramater] " << tname << " needs 2 parameters!" << std::endl;
      return false;
    }
    auto it = values.begin();
    MT  = *it; if(this->check(tname, "MT", *it, "MetadataMSIBase", false)) return false; it++;
    DT  = *it; if(this->check(tname, "DT", *it, "CMDataBase", true)) return false; it++;
    return true;
  }
  
  virtual void emit(std::ofstream &file) {
    file << "typedef " << tname << "<" << MT << "," << DT << "> " << this->name << ";" << std::endl;
  }    
};

class TypeInnerPortMSIBroadcast : public TypeInnerCohPortBase
{
  std::string MT, DT;
  const std::string tname;
public:
  TypeInnerPortMSIBroadcast(const std::string &name) : TypeInnerCohPortBase(name), tname("InnerPortMSIBroadcast") {}

  virtual bool set(std::list<std::string> &values) {
    if(values.size() != 2) {
      std::cerr << "[Misnatching Paramater] " << tname << " needs 2 parameters!" << std::endl;
      return false;
    }
    auto it = values.begin();
    MT  = *it; if(this->check(tname, "MT", *it, "MetadataMSIBase", false)) return false; it++;
    DT  = *it; if(this->check(tname, "DT", *it, "CMDataBase", true)) return false; it++;
    return true;
  }
  
  virtual void emit(std::ofstream &file) {
    file << "typedef " << tname << "<" << MT << "," << DT << "> " << this->name << ";" << std::endl;
  }    
};

class TypeCoreInterfaceBase : public TypeInnerCohPortBase {
public: TypeCoreInterfaceBase(const std::string &name) : TypeInnerCohPortBase(name) { types.insert("CoreInterfaceBase"); }
};

class TypeCoreInterfaceMSI : public TypeCoreInterfaceBase
{
  std::string MT, DT;
  const std::string tname;
public:
  TypeCoreInterfaceMSI(const std::string &name) : TypeCoreInterfaceBase(name), tname("CoreInterfaceMSI") {}

  virtual bool set(std::list<std::string> &values) {
    if(values.size() != 2) {
      std::cerr << "[Misnatching Paramater] " << tname << " needs 2 parameters!" << std::endl;
      return false;
    }
    auto it = values.begin();
    MT  = *it; if(this->check(tname, "MT", *it, "MetadataMSIBase", false)) return false; it++;
    DT  = *it; if(this->check(tname, "DT", *it, "CMDataBase", true)) return false; it++;
    return true;
  }
  
  virtual void emit(std::ofstream &file) {
    file << "typedef " << tname << "<" << MT << "," << DT << "> " << this->name << ";" << std::endl;
  }    
};

class TypeCoherentCacheBase : public Description {
public: TypeCoherentCacheBase(const std::string &name) : Description(name) { types.insert("CoherentCacheBase"); }
};

class TypeCoherentCacheNorm : public TypeCoherentCacheBase
{
  std::string CacheT, OuterT, InnerT;
  const std::string tname;
public:
  TypeCoherentCacheNorm(const std::string &name) : TypeCoherentCacheBase(name), tname("CoherentCacheNorm") {}

  virtual bool set(std::list<std::string> &values) {
    if(values.size() != 3) {
      std::cerr << "[Misnatching Paramater] " << tname << " needs 3 parameters!" << std::endl;
      return false;
    }
    auto it = values.begin();
    CacheT = *it; if(this->check(tname, "CacheT", *it, "CacheBase", false)) return false; it++;
    OuterT = *it; if(this->check(tname, "OuterT", *it, "OuterCohPortBase", false)) return false; it++;
    InnerT = *it; if(this->check(tname, "InnerT", *it, "InnerCohPortBase", false)) return false; it++;
    return true;
  }
  
  virtual void emit(std::ofstream &file) {
    file << "typedef " << tname << "<" << CacheT << "," << OuterT << "," << InnerT << "> " << this->name << ";" << std::endl;
  }
};

class TypeCoherentL1CacheNorm : public TypeCoherentCacheBase
{
  std::string CacheT, OuterT, CoreT;
  const std::string tname;
public:
  TypeCoherentL1CacheNorm(const std::string &name) : TypeCoherentCacheBase(name), tname("CoherentL1CacheNorm") {}

  virtual bool set(std::list<std::string> &values) {
    if(values.size() != 3) {
      std::cerr << "[Misnatching Paramater] " << tname << " needs 3 parameters!" << std::endl;
      return false;
    }
    auto it = values.begin();
    CacheT = *it; if(this->check(tname, "CacheT", *it, "CacheBase", false)) return false; it++;
    OuterT = *it; if(this->check(tname, "OuterT", *it, "OuterCohPortBase", false)) return false; it++;
    CoreT = *it; if(this->check(tname, "CoreT", *it, "CoreInterfaceBase", false)) return false; it++;
    return true;
  }
  
  virtual void emit(std::ofstream &file) {
    file << "typedef " << tname << "<" << CacheT << "," << OuterT << "," << CoreT << "> " << this->name << ";" << std::endl;
  }
};

class TypeCoherentLLCNorm : public TypeCoherentCacheBase
{
  std::string CacheT, OuterT, InnerT;
  const std::string tname;
public:
  TypeCoherentLLCNorm(const std::string &name) : TypeCoherentCacheBase(name), tname("CoherentLLCNorm") {}

  virtual bool set(std::list<std::string> &values) {
    if(values.size() != 3) {
      std::cerr << "[Misnatching Paramater] " << tname << " needs 3 parameters!" << std::endl;
      return false;
    }
    auto it = values.begin();
    CacheT = *it; if(this->check(tname, "CacheT", *it, "CacheBase", false)) return false; it++;
    OuterT = *it; if(this->check(tname, "OuterT", *it, "OuterCohPortBase", false)) return false; it++;
    InnerT = *it; if(this->check(tname, "InnerT", *it, "InnerCohPortBase", false)) return false; it++;
    return true;
  }
  
  virtual void emit(std::ofstream &file) {
    file << "typedef " << tname << "<" << CacheT << "," << OuterT << "," << InnerT << "> " << this->name << ";" << std::endl;
  }
};

////////////////////////////// Index ///////////////////////////////////////////////

class TypeIndexFuncBase : public Description {
public: TypeIndexFuncBase(const std::string &name) : Description(name) { types.insert("IndexFuncBase"); }
};

class TypeIndexNorm : public TypeIndexFuncBase
{
  int IW, IOfst;
  const std::string tname;
public:
  TypeIndexNorm(const std::string &name) : TypeIndexFuncBase(name), tname("IndexNorm") {}

  virtual bool set(std::list<std::string> &values) {
    if(values.size() != 2) {
      std::cerr << "[Misnatching Paramater] " << tname << " needs 2 parameters!" << std::endl;
      return false;
    }
    auto it = values.begin();
    IW = std::stoi(*it); it++;
    IOfst = std::stoi(*it); it++;
    return true;
  }
  
  virtual void emit(std::ofstream &file) {
    file << "typedef " << tname << "<" << IW << "," << IOfst << "> " << this->name << ";" << std::endl;
  }  
};

class TypeIndexSkewed : public TypeIndexFuncBase
{
  int IW, IOfst, P;
  const std::string tname;
public:
  TypeIndexSkewed(const std::string &name) : TypeIndexFuncBase(name), tname("IndexSkewed") {}

  virtual bool set(std::list<std::string> &values) {
    if(values.size() != 3) {
      std::cerr << "[Misnatching Paramater] " << tname << " needs 3 parameters!" << std::endl;
      return false;
    }
    auto it = values.begin();
    IW = std::stoi(*it); it++;
    IOfst = std::stoi(*it); it++;
    P = std::stoi(*it); it++;
    return true;
  }
  
  virtual void emit(std::ofstream &file) {
    file << "typedef " << tname << "<" << IW << "," << IOfst << "," << P << "> " << this->name << ";" << std::endl;
  }  
};

class TypeIndexRandom : public TypeIndexFuncBase
{
  int IW, IOfst;
  const std::string tname;
public:
  TypeIndexRandom(const std::string &name) : TypeIndexFuncBase(name), tname("IndexRandom") {}

  virtual bool set(std::list<std::string> &values) {
    if(values.size() != 2) {
      std::cerr << "[Misnatching Paramater] " << tname << " needs 2 parameters!" << std::endl;
      return false;
    }
    auto it = values.begin();
    IW = std::stoi(*it); it++;
    IOfst = std::stoi(*it); it++;
    return true;
  }
  
  virtual void emit(std::ofstream &file) {
    file << "typedef " << tname << "<" << IW << "," << IOfst << "> " << this->name << ";" << std::endl;
  }  
};

////////////////////////////// Replacer ///////////////////////////////////////////////

class TypeReplaceFuncBase : public Description {
public: TypeReplaceFuncBase(const std::string &name) : Description(name) { types.insert("ReplaceFuncBase"); }
};

class TypeReplaceFIFO : public TypeReplaceFuncBase
{
  int IW, NW;
  const std::string tname;
public:
  TypeReplaceFIFO(const std::string &name) : TypeReplaceFuncBase(name), tname("ReplaceFIFO") {}

  virtual bool set(std::list<std::string> &values) {
    if(values.size() != 2) {
      std::cerr << "[Misnatching Paramater] " << tname << " needs 2 parameters!" << std::endl;
      return false;
    }
    auto it = values.begin();
    IW = std::stoi(*it); it++;
    NW = std::stoi(*it); it++;
    return true;
  }
  
  virtual void emit(std::ofstream &file) {
    file << "typedef " << tname << "<" << IW << "," << NW << "> " << this->name << ";" << std::endl;
  }  
};

class TypeReplaceLRU : public TypeReplaceFuncBase
{
  int IW, NW;
  const std::string tname;
public:
  TypeReplaceLRU(const std::string &name) : TypeReplaceFuncBase(name), tname("ReplaceLRU") {}

  virtual bool set(std::list<std::string> &values) {
    if(values.size() != 2) {
      std::cerr << "[Misnatching Paramater] " << tname << " needs 2 parameters!" << std::endl;
      return false;
    }
    auto it = values.begin();
    IW = std::stoi(*it); it++;
    NW = std::stoi(*it); it++;
    return true;
  }
  
  virtual void emit(std::ofstream &file) {
    file << "typedef " << tname << "<" << IW << "," << NW << "> " << this->name << ";" << std::endl;
  }  
};

#endif
