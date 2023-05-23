#include "dsl/statement.hpp"
#include "dsl/type_description.hpp"
#include "dsl/entity.hpp"

CodeGen::CodeGen() {
  typedb.init();

  decoders.push_back(new StatementComment);
  decoders.push_back(new StatementBlank);
  decoders.push_back(new StatementNameSpace);
  decoders.push_back(new StatementConst);
  decoders.push_back(new StatementTypeDef);
  decoders.push_back(new StatementCreate);

  decoders.push_back(new StatementError); // always the final one

}

CodeGen::~CodeGen() {
  for(auto d:decoders) delete d;
}

bool CodeGen::parse_int(const std::string &param, int &rv) {
  if(consts.count(param)) {
    rv = consts[param];
    return true;
  }

  try { rv = std::stoi(param); }
  catch(std::invalid_argument &e) {
    std::cerr << "[Integer] Fail to parse `" << param << "' into an integer." << std::endl;
    return false;
  }

  return true;
}

bool CodeGen::parse_bool(const std::string &param, bool &rv) {
  if(param == "true"  || param == "TRUE" || param == "T") { rv = true; return true; }
  if(param == "false" || param == "FALSE" || param == "F") { rv = false; return true; }
  
  if(consts.count(param)) {
    rv = consts[param];
    return true;
  }

  try { rv = std::stoi(param); }
  catch(std::invalid_argument &e) {
    std::cerr << "[Integer] Fail to parse `" << param << "' into boolean." << std::endl;
    return false;
  }

  return true;
}

void CodeGen::emit_hpp(std::ofstream &file) {
  file << "#include <vector>" << std::endl;
  for(auto h:header_list) file << "#include \"" << h << "\"" << std::endl;
  file << std::endl;
  if(!space.empty()) file << "namespace " << space << " {\n" << std::endl;
  for(auto def:type_declarations) def->emit(file);
  for(auto e:entities) e->emit_declaration(file, true);
  if(!space.empty()) file << "\n}" << std::endl;
}

void CodeGen::emit_cpp(std::ofstream &file, const std::string& h) {
  file << "#include \"" << h << "\"" << std::endl;
  if(!space.empty()) file << "namespace " << space << " {\n" << std::endl;
  for(auto e:entities) e->emit_declaration(file, false);
  file << "void init() {" << std::endl;
  for(auto e:entities) e->emit_initialization(file);
  file << "}" << std::endl;
  if(!space.empty()) file << "\n}" << std::endl;
}

CodeGen codegendb;

bool StatementComment::decode(const char* line) {
  if(!std::regex_match(line, cm, expression)) return false;
  return true; // do nothing
}

bool StatementBlank::decode(const char* line) {
  if(!std::regex_match(line, cm, expression)) return false;
  return true; // do nothing
}

bool StatementTypeDef::decode(const char* line) {
  if(!std::regex_match(line, cm, expression)) return false;

  // get the param list
  char param[2048];
  std::list<std::string> params;
  strcpy(param, std::string(cm[3]).c_str());
  char *p = strtok(param, " ,");
  while(p != NULL) { if(strlen(p)) params.push_back(std::string(p)); p = strtok(NULL, " ,"); }

  if(typedb.create(cm[1], cm[2], params)) return true;

  // report as failed to decode
  std::cerr << "[Decode] Cannot decode "
            << "type " << cm[1] << " = " << cm[2] << "(";
  auto it = params.begin();
  for(int i=0; i<params.size()-1; i++, it++)
    std::cerr << *it << ",";
  if(params.size() > 0) std::cerr << *it;
  std::cerr << ")" << std::endl;
  return false;
}

bool StatementConst::decode(const char* line) {
  if(!std::regex_match(line, cm, expression)) return false;

  if(codegendb.consts.count(cm[1])) {
    std::cerr << "[Double Definition] Const `" << cm[1] << "' has already been defined!" << std::endl;
    return false;
  }

  try { codegendb.consts[cm[1]] = std::stoi(cm[2]); }
  catch(std::invalid_argument &e) {
    std::cerr << "[Integer] Fail to parse `" << cm[2] << "' into integer for defining const `" << cm[1] << "'." << std::endl;
    return false;
  }

  return true;
}

bool StatementCreate::decode(const char* line) {
  if(!std::regex_match(line, cm, expression)) return false;

  std::string name(cm[1]);
  std::string type_name(cm[2]);
  int size;
  if(!codegendb.parse_int(cm[3], size)) return false;
  entitydb.create(name, type_name, size);
  return true;
}

bool StatementNameSpace::decode(const char* line) {
  if(!std::regex_match(line, cm, expression)) return false;
  codegendb.space = cm[1];
  return true;
}
