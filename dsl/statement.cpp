#include "dsl/statement.hpp"
#include "dsl/type_description.hpp"

CodeGen::CodeGen() {
  decoders.push_back(new StatementComment);
  decoders.push_back(new StatementBlank);
  decoders.push_back(new StatementTypeDef0);
  decoders.push_back(new StatementTypeDef1);

  decoders.push_back(new StatementError); // always the final one
}

CodeGen::~CodeGen() {
  for(auto d:decoders) delete d;
}

void CodeGen::emit(std::ofstream &file) {
  for(auto h:header_list) file << "#include \"" << h << "\"" << std::endl;
  file << std::endl;
  for(auto def:type_declarations) def->emit(file);
}

CodeGen codegendb;
