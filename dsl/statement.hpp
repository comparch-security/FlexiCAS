#ifndef CM_DSL_STATEMENT_HPP
#define CM_DSL_STATEMENT_HPP

#include <set>
#include <map>
#include <list>
#include <regex>
#include <iostream>
#include <fstream>
#include <cstdlib>
#include <cstring>

#include "dsl/type_description.hpp"

// base class for declaring a type definition
class TypeDeclaration
{
public:
  virtual void emit(std::ofstream &file) = 0;
};

// base class for processing a statement
class StatementBase
{
protected:
  const std::regex expression;
public:
  StatementBase(const std::string& exp): expression(exp) {}
  virtual ~StatementBase() {}

  // try to decode the line
  virtual bool decode(const char* line) = 0;
};


// comment
class StatementComment : public StatementBase
{
public:
  StatementComment() : StatementBase("^\\s*//.*$") {}

  virtual bool decode(const char* line) {
    std::cmatch cm;
    if(std::regex_match(line, cm, expression)) {
      std::cout << "Comment: " << std::string(line) << std::endl;
      return true;
    }
    else return false;
  }
};

// blank line
class StatementBlank : public StatementBase
{
public:
  StatementBlank() : StatementBase("^\\s*$") {}

  virtual bool decode(const char* line) {
    std::cmatch cm;
    if(std::regex_match(line, cm, expression)) {
      std::cout << "Blank: " << std::string(line) << std::endl;
      return true;
    }
    else return false;
  }
};

// blank line
class StatementTypeDef0 : public StatementBase
{
public:
  StatementTypeDef0() : StatementBase("^\\s*type\\s+([a-z0-9_]+)\\s*=\\s*([a-z0-9_]+)\\s*;((\\s*//.*)|(\\s*))$") {}

  virtual bool decode(const char* line) {
    std::cmatch cm;
    if(std::regex_match(line, cm, expression)) {
      std::cout << "Type def " << cm[1] << " = " << cm[2]  << std::endl;
      return true;
    }
    else return false;
  }
};

// blank line
class StatementTypeDef1 : public StatementBase
{
public:
  //StatementTypeDef1() : StatementBase("^[(]([a-z0-9_]+)[)]$") {}
  StatementTypeDef1() : StatementBase("^\\s*type\\s+([a-z0-9_]+)\\s*=\\s*([a-z0-9_]+)\\s*[(]([a-z0-9_, ]*)[)]\\s*;((\\s*//.*)|(\\s*))$") {}

  virtual bool decode(const char* line) {
    std::cmatch cm;
    if(std::regex_match(line, cm, expression)) {
      char param[2048];
      std::list<std::string> params;
      strcpy(param, std::string(cm[3]).c_str());
      char *p = strtok(param, " ,");
      while(p != NULL) { if(strlen(p)) params.push_back(std::string(p)); p = strtok(NULL, " ,"); }
      std::cout << "Type def " << std::string(cm[1]) << " = " << cm[2] << ": "; for(auto pp:params) std::cout << pp << " "; std::cout << std::endl;
      return true;
    }
    else return false;
  }
};

// blank line
class StatementError : public StatementBase
{
public:
  StatementError() : StatementBase(".*") {}

  virtual bool decode(const char* line) {
    std::cerr << "Error: cannot parse line: " << std::endl;
    std::cerr << std::string(line) << std::endl;
    exit(-1);
  }
};

struct CodeGen
{
  std::list<StatementBase *> decoders;
  
  std::set<std::string> header_set;
  std::list<std::string> header_list;
  
  std::list<TypeDeclaration *> type_declarations;

  CodeGen() {
    decoders.push_back(new StatementComment);
    decoders.push_back(new StatementBlank);
    decoders.push_back(new StatementTypeDef0);
    decoders.push_back(new StatementTypeDef1);

    decoders.push_back(new StatementError); // always the final one
  }

  ~CodeGen() {
    for(auto d:decoders) delete d;
  }

  void emit(std::ofstream &file) {
    for(auto h:header_list) file << "#include \"" << h << "\"" << std::endl;
    file << std::endl;
    for(auto def:type_declarations) def->emit(file);
  }
};

extern CodeGen codegendb;

#endif
