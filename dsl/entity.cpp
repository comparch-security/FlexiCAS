#include "dsl/entity.hpp"
#include "dsl/type_description.hpp"
#include "dsl/statement.hpp"

void CacheEntity::emit_declaration(std::ofstream &file, bool hpp) {
  if(hpp) file << "extern ";
  file << "std::vector<" << etype->name << " *> " << name;
  if(hpp)  file << ";" << std::endl;
  else     file << "(" << size << ");" << std::endl;
}

void CacheEntity::emit_initialization(std::ofstream &file) {
  file << "  for(int i=0; i<" << size << "; i++) "
       << name << "[i] = new " << etype->name << "(std::string(\"" << name << "\") + \"_\" + std::to_string(i));" << std::endl;
}

EntityDB::~EntityDB() {
  for(auto e:entities) delete e.second;
}

bool EntityDB::create(const std::string &name, const std::string &etype, std::list<std::string> &params,  unsigned int size) {
  if(!typedb.types.count(etype)) {
    std::cerr << "[Decode] Fail to match `" << etype << "' with a known type." << std::endl;
    return false;
  }
  auto e = new CacheEntity(typedb.types[etype], name, params, size);
  if(!add(name, e)) return false;
  codegendb.entities.push_back(e);
  return true;
}
