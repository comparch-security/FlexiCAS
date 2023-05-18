#include "dsl/type_description.hpp"

DescriptionDB::DescriptionDB() {
  // adding types with no template parameters
  add("Data64B", new TypeData64B());
}

DescriptionDB::~DescriptionDB() {
  for(auto t:types) delete t.second;
}

DescriptionDB typedb;
