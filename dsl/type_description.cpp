#include "dsl/type_description.hpp"

DescriptionDB::DescriptionDB() {
  Description *descriptor;
  // adding types with no template parameters
  descriptor = new TypeData64B(); add("Data64B", descriptor); descriptor.emit_header();
}

DescriptionDB::~DescriptionDB() {
  for(auto t:types) delete t.second;
}

bool DescriptionDB::create(const std::string &type_name, const std::string &base_name, std::list<std::string> &params) {
  Description *descriptor;
  if(base_name == "MetadataMSI") descriptor = new TypeMetadataMSI(type_name);

  if(!descriptor.set(params)) return false;
  add(type_name, descriptor);
  descriptor.emit_header();
  codegenbd.type_declarations.push_back(descriptor);
}

DescriptionDB typedb;

