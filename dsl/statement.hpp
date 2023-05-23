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

// base class for processing a statement
class StatementBase
{
protected:
  const std::regex expression;
  std::cmatch cm;
public:
  StatementBase(const std::string& exp): expression(exp) {}
  virtual ~StatementBase() {}

  // try to decode the line
  virtual bool decode(const char* line) = 0;
};

class Description;
class CacheEntity;

struct CodeGen
{
  std::string space;
  std::list<StatementBase *> decoders;
  std::set<std::string> header_set;
  std::list<std::string> header_list;
  std::list<Description *> type_declarations;
  std::list<CacheEntity *> entities;
  std::map<std::string, int> consts;

  CodeGen();
  ~CodeGen();

  void add_header(const std::string& header) {
    if(!header_set.count(header)) {
      header_set.insert(header);
      header_list.push_back(header);
    }
  }

  bool parse_int(const std::string &param, int &rv);
  bool parse_bool(const std::string &param, bool &rv);

  void emit_hpp(std::ofstream &file);
  void emit_cpp(std::ofstream &file, const std::string& h);
};

extern CodeGen codegendb;

// comment
class StatementComment : public StatementBase
{
public:
  StatementComment() : StatementBase("^\\s*//.*$") {}
  virtual bool decode(const char* line);
};

// blank line
class StatementBlank : public StatementBase
{
public:
  StatementBlank() : StatementBase("^\\s*$") {}
  virtual bool decode(const char* line);
};

// type definition
class StatementTypeDef : public StatementBase
{
public:
  StatementTypeDef() : StatementBase("^\\s*type\\s+([a-zA-Z0-9_]+)\\s*=\\s*([a-zA-Z0-9_]+)\\s*[(]([a-zA-Z0-9_, ]*)[)]\\s*;((\\s*//.*)|(\\s*))$") {}
  virtual bool decode(const char* line);
};

// set a const variable for integer value
class StatementConst : public StatementBase
{
public:
  StatementConst() : StatementBase("^\\s*const\\s+([a-zA-Z0-9_]+)\\s*=\\s*([a-zA-Z0-9_]+)\\s*;((\\s*//.*)|(\\s*))$") {}
  virtual bool decode(const char* line);
};

// create entities
class StatementCreate : public StatementBase
{
public:
  StatementCreate() : StatementBase("^\\s*create\\s+([a-zA-Z0-9_]+)\\s*=\\s*([a-zA-Z0-9_]+)\\s*[(]([a-zA-Z0-9_]+)[)]\\s*;((\\s*//.*)|(\\s*))$") {}
  virtual bool decode(const char* line);
};

// create entities
class StatementNameSpace : public StatementBase
{
public:
  StatementNameSpace() : StatementBase("^\\s*namespace\\s+([a-zA-Z0-9_]+)\\s*;((\\s*//.*)|(\\s*))$") {}
  virtual bool decode(const char* line);
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

#endif
