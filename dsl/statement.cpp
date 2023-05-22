#include "dsl/statement.hpp"
#include "dsl/type_description.hpp"

CodeGen::CodeGen() {
  typedb.init();
  
  decoders.push_back(new StatementComment);
  decoders.push_back(new StatementBlank);
  decoders.push_back(new StatementConst);
  decoders.push_back(new StatementTypeDef);

  decoders.push_back(new StatementError); // always the final one
}

CodeGen::~CodeGen() {
  for(auto d:decoders) delete d;
}

void CodeGen::emit_hpp(std::ofstream &file) {
  for(auto h:header_list) file << "#include \"" << h << "\"" << std::endl;
  file << std::endl;
  for(auto def:type_declarations) def->emit(file);
}

void CodeGen::emit_cpp(std::ofstream &file) {
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
