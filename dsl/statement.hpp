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
struct StatementBase
{
protected:
  const std::regex expression;
  std::cmatch cm;
public:
  StatementBase(const std::string& exp): expression(exp) {}
  virtual ~StatementBase() {}

  // try to decode the line
  bool match(const char* line);
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
  std::list<std::pair<std::pair<CacheEntity *, int>, std::pair<CacheEntity *, int> > > connections;

  bool debug;

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

#define GEN_STATEMENT(s) struct Statement ## s : public StatementBase { Statement ## s(); virtual bool decode(const char* line); }

GEN_STATEMENT(Blank);
GEN_STATEMENT(Comment);
GEN_STATEMENT(NameSpace);
GEN_STATEMENT(Const);
GEN_STATEMENT(TypeDef);
GEN_STATEMENT(Create);
GEN_STATEMENT(Connect);
GEN_STATEMENT(Error);

#undef GEN_STATEMENT

#endif
