#ifndef CM_DSL_ENTITY_HPP
#define CM_DSL_ENTITY_HPP

#include <iostream>
#include <fstream>
#include <map>

class Description;

// class to record the created entities
struct CacheEntity
{
  Description *etype;
  const std::string name;
  const unsigned int size;
  CacheEntity(Description *etype, const std::string &name, unsigned int size)
    : etype(etype), name(name), size(size) {}

  void emit_declaration(std::ofstream &file, bool hpp);
  void emit_initialization(std::ofstream &file);
};

struct EntityDB
{
  ~EntityDB();
  std::map<std::string, CacheEntity *> entities;
  bool create(const std::string &name, const std::string &etype,  unsigned int size);
  bool add(const std::string &name, CacheEntity *e) {
    if(entities.count(name)) {
      std::cerr << "[Double Definition] Object `" << name << "' has already been defined!" << std::endl;
      return false;
    }
    entities[name] = e;
    return true;
  }
};

extern EntityDB entitydb;

#endif
