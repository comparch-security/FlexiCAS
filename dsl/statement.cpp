#include "dsl/statement.hpp"
#include "dsl/type_description.hpp"
#include "dsl/entity.hpp"

namespace {
  const std::string R_LS   = "^\\s*";                   // line start
  const std::string R_LE   = "(//.*)?\\s*$";            // line end
  const std::string R_SE   = ";\\s*"+R_LE;              // statement end
  const std::string R_VAR  = "\\s*([a-zA-Z0-9_]+)\\s*"; // variable (or a const number)
  const std::string R_ARGL = "\\s*([(](.*)[)])?\\s*";      // arguement list, need a 2nd level parsing
  const std::string R_R    = R_VAR+"(:"+R_VAR+")?";     // range
  const std::string R_SI   = "(\\["+R_VAR+"])?\\s*";    // single index
  const std::string R_RI   = "(\\["+R_R+"])?\\s*";      // range index

  void parse_arglist(const char *plist, std::list<std::string> & params) {
    static char param[2048];
    strcpy(param, plist);
    char *p = strtok(param, " ,");
    while(p != NULL) { if(strlen(p)) params.push_back(std::string(p)); p = strtok(NULL, " ,"); }
  }
}

void CodeGen::init() {

  decoders.push_back(new StatementBlank);
  decoders.push_back(new StatementComment);
  decoders.push_back(new StatementNameSpace);
  decoders.push_back(new StatementConst);
  decoders.push_back(new StatementTypeVoid);
  decoders.push_back(new StatementTypeDef);
  decoders.push_back(new StatementCreate);
  decoders.push_back(new StatementConnect);
  decoders.push_back(new StatementDispatch);

  decoders.push_back(new StatementError); // always the final one

  // add boolean consts
  consts["true"] = 1;
  consts["TRUE"] = 1;
  consts["false"] = 0;
  consts["FALSE"] = 0;

  debug = false;
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

void CodeGen::emit_hpp(std::ofstream &file) {
  file << "#include <vector>" << std::endl;
  file << std::endl;
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
  file << std::endl;
  file << "void init() {" << std::endl;
  file << std::endl;
  file << "  // initialize entities" << std::endl;
  for(auto e:entities) e->emit_initialization(file);
  file << std::endl;
  file << "  // connect entities" << std::endl;
  for(auto c:connections) {
    auto client = c.first.first;
    int ci = c.first.second;
    auto manager = c.second.first;
    int mi = c.second.second;
    file << "  " << client->name << "[" << ci << "]" << client->etype->get_outer() << "->connect(";
    file << manager->name << "[" << mi << "]" << manager->etype->get_inner() << ", ";
    file << manager->name << "[" << mi << "]" << manager->etype->get_inner() << "->connect(";
    file << client->name << "[" << ci << "]" << client->etype->get_outer() << "));" << std::endl;
  }

  file << std::endl;
  file << "  // connect by dispatch" << std::endl;
  for(auto c:dispatch_outer) {
    auto dispatcher = c.first;
    auto manager = c.second.first;
    int mi = c.second.second;
    file << "  " << dispatcher->name << "[0]->connect(";
    file << manager->name << "[" << mi << "]" << manager->etype->get_inner() << ");" << std::endl;
  }
  for(auto c:dispatch_connections) {
    auto dispatcher = std::get<0>(c);
    auto client = std::get<1>(c).first;
    int ci = std::get<1>(c).second;
    auto manager = std::get<2>(c).first;
    int mi = std::get<2>(c).second;
    file << "  " << client->name << "[" << ci << "]" << client->etype->get_outer() << "->connect(";
    file << dispatcher->name << "[0] , ";
    file << manager->name << "[" << mi << "]" << manager->etype->get_inner() << "->connect(";
    file << client->name << "[" << ci << "]" << client->etype->get_outer() << "));" << std::endl;
  }
  for(auto c:dispatch_inner) {
    auto client = c.first.first;
    int ci = c.first.second;
    auto manager = c.second.first;
    int mi = c.second.second;
    file << "  " << manager->name << "[" << mi << "]" << manager->etype->get_inner() << "->connect(";
    file << client->name << "[" << ci << "]" << client->etype->get_outer() << ");" << std::endl;
  }
  file << std::endl;  
  file << "}" << std::endl;
  if(!space.empty()) file << "\n}" << std::endl;
}

bool StatementBase::match(const char* line) {
  if(!std::regex_match(line, cm, expression)) return false;
  if(codegendb.debug) {
    std::cout << line << std::endl;
    int i=0; for(auto m: cm) std::cout << "cm[" << i++ << "]: " << m << std::endl;
    std::cout << std::endl;
  }
  return true;
}

StatementBlank::StatementBlank() : StatementBase(R_LS+R_LE) {}

bool StatementBlank::decode(const char* line) {
  return match(line); // doing nothing
}

StatementComment::StatementComment() : StatementBase(R_LS+R_SE) {}

bool StatementComment::decode(const char* line) {
  return match(line); // doing nothing
}

StatementTypeVoid::StatementTypeVoid() : StatementBase(R_LS+"type"+R_VAR+"="+"\\s*void\\s*"+R_SE) {}

bool StatementTypeVoid::decode(const char* line) {
  if(!match(line)) return false;
  std::list<std::string> params;
  if(typedb.create(cm[1], "void", params)) return true;

  // report as failed to decode
  std::cerr << "[Decode] Cannot decode: type " << cm[1] << " = " << "void";
  std::cerr << std::endl;
  return false;
}


StatementTypeDef::StatementTypeDef() : StatementBase(R_LS+"type"+R_VAR+"="+R_VAR+R_ARGL+R_SE) {}

bool StatementTypeDef::decode(const char* line) {
  if(!match(line)) return false;
  std::list<std::string> params;
  if(cm[3].length()) parse_arglist(std::string(cm[4]).c_str(), params);
  if(typedb.create(cm[1], cm[2], params)) return true;

  // report as failed to decode
  std::cerr << "[Decode] Cannot decode: type " << cm[1] << " = " << cm[2] << "(";
  int i=1; for(auto p:params) std::cerr << p << (params.size() == i++ ? ")" : ",");
  std::cerr << std::endl;
  return false;
}

StatementConst::StatementConst() : StatementBase(R_LS+"const"+R_VAR+"="+R_VAR+R_SE) {}

bool StatementConst::decode(const char* line) {
  if(!match(line)) return false;

  std::string name(cm[1]);
  if(codegendb.consts.count(name)) {
    std::cerr << "[Double Definition] Const `" << name << "' has already been defined!" << std::endl;
    return false;
  }

  if(!codegendb.parse_int(cm[2], codegendb.consts[cm[1]])) return false;
  return true;
}

StatementCreate::StatementCreate() : StatementBase(R_LS+"create"+R_VAR+"="+R_VAR+R_ARGL+R_SI+R_SE) {}

bool StatementCreate::decode(const char* line) {
  if(!match(line)) return false;

  std::string name(cm[1]);
  std::string type_name(cm[2]);
  std::list<std::string> params;
  int size = 1; // default
  if(cm[3].length()) parse_arglist(std::string(cm[4]).c_str(), params);
  if(cm[5].length() && !codegendb.parse_int(cm[6], size)) return false;
  entitydb.create(name, type_name, params, size);
  return true;
}

StatementNameSpace::StatementNameSpace() : StatementBase(R_LS+"namespace"+R_VAR+R_SE) {}

bool StatementNameSpace::decode(const char* line) {
  if(!match(line)) return false;
  codegendb.space = cm[1];
  return true;
}

StatementConnect::StatementConnect() : StatementBase(R_LS+"connect"+R_VAR+R_RI+"->"+R_VAR+R_SI+R_SE) {}

bool StatementConnect::decode(const char* line) {
  if(!match(line)) return false;

  // get client
  std::string client(cm[1]);
  if(!entitydb.entities.count(client)) {
    std::cerr << "[Decode] Fail to match `" << client << "' with a created entity." << std::endl;
  }
  auto client_entity = entitydb.entities[client];

  // client range
  int r0 = 0, r1 = 0;
  if(cm[2].length()) { // has start range
    if(!codegendb.parse_int(cm[3], r0)) return false;
    if(r0 < 0 || r0 >= client_entity->size) {
      std::cerr << "[Decode] " << cm[2] << " out of the valid range [" << client_entity->size-1 << ":0] of " << client << std::endl;
      return false;
    }

    if(cm[4].length()) { // has end range
      if(!codegendb.parse_int(cm[5], r1)) return false;
      if(r0 < r1 || r1 >= client_entity->size) {
        std::cerr << "[Decode] " << cm[2] << " out of the valid range [" << client_entity->size-1 << ":0] of " << client << std::endl;
        return false;
      }
    } else r1 = r0;
  } else { // no start range
    r0 = client_entity->size-1;
  }

  // get manager
  std::string manager(cm[6]);
  if(!entitydb.entities.count(manager)) {
    std::cerr << "[Decode] Fail to match `" << manager << "' with a created entity." << std::endl;
  }
  auto manager_entity = entitydb.entities[manager];

  // manager index
  int mi = 0;
  if(cm[7].length() && !codegendb.parse_int(cm[8], mi)) return false;
  if(mi < 0 || mi >= manager_entity->size) {
    std::cerr << "[Decode] " << cm[7] << " out of the valid range [" << manager_entity->size-1 << ":0] of " << manager << std::endl;
    return false;
  }

  for(int i=r1; i<=r0; i++)
    codegendb.connections.push_back(std::make_pair(std::make_pair(client_entity, i), std::make_pair(manager_entity, mi)));

  return true;
}

StatementDispatch::StatementDispatch() : StatementBase(R_LS+"dispatch"+R_VAR+":"+R_VAR+R_RI+"->"+R_VAR+R_RI+R_SE) {}

bool StatementDispatch::decode(const char* line) {
  if(!match(line)) return false;

  // get dispatcher
  std::string dispatcher(cm[1]);
  if(!entitydb.entities.count(dispatcher)) {
    std::cerr << "[Decode] Fail to match `" << dispatcher << "' with a created entity." << std::endl;
  }
  auto dispatcher_entity = entitydb.entities[dispatcher];

  // get client
  std::string client(cm[2]);
  if(!entitydb.entities.count(client)) {
    std::cerr << "[Decode] Fail to match `" << client << "' with a created entity." << std::endl;
  }
  auto client_entity = entitydb.entities[client];

  // get client range
  int c0 = 0, c1 = 0;
  if(cm[3].length()) { // has start range
    if(!codegendb.parse_int(cm[4], c0)) return false;
    if(c0 < 0 || c0 >= client_entity->size) {
      std::cerr << "[Decode] " << cm[3] << " out of the valid range [" << client_entity->size-1 << ":0] of " << client << std::endl;
      return false;
    }

    if(cm[5].length()) { // has end range
      if(!codegendb.parse_int(cm[6], c1)) return false;
      if(c0 < c1 || c1 >= client_entity->size) {
        std::cerr << "[Decode] " << cm[3] << " out of the valid range [" << client_entity->size-1 << ":0] of " << client << std::endl;
        return false;
      }
    } else c1 = c0;
  } else { // no start range
    c0 = client_entity->size-1;
  }

  // get manager
  std::string manager(cm[7]);
  if(!entitydb.entities.count(manager)) {
    std::cerr << "[Decode] Fail to match `" << manager << "' with a created entity." << std::endl;
  }
  auto manager_entity = entitydb.entities[manager];

  // get client range
  int m0 = 0, m1 = 0;
  if(cm[8].length()) { // has start range
    if(!codegendb.parse_int(cm[9], m0)) return false;
    if(m0 < 0 || m0 >= manager_entity->size) {
      std::cerr << "[Decode] " << cm[8] << " out of the valid range [" << manager_entity->size-1 << ":0] of " << manager << std::endl;
      return false;
    }

    if(cm[10].length()) { // has end range
      if(!codegendb.parse_int(cm[11], m1)) return false;
      if(m0 < m1 || m1 >= manager_entity->size) {
        std::cerr << "[Decode] " << cm[8] << " out of the valid range [" << manager_entity->size-1 << ":0] of " << manager << std::endl;
        return false;
      }
    } else m1 = m0;
  } else { // no start range
    m0 = manager_entity->size-1;
  }

  // add to emitter
  for(int i=m1; i<=m0; i++)
    codegendb.dispatch_outer.push_back(std::make_pair(dispatcher_entity, std::make_pair(manager_entity, i)));

  for(int i=c1; i<=c0; i++)
    codegendb.dispatch_connections.push_back(std::make_tuple(dispatcher_entity, std::make_pair(client_entity, i), std::make_pair(manager_entity, m1)));

  for(int i=m1+1; i<=m0; i++)
    for(int j=c1; j<=c0; j++)
      codegendb.dispatch_inner.push_back(std::make_pair(std::make_pair(client_entity, j), std::make_pair(manager_entity, i)));

  return true;
}

StatementError::StatementError() :  StatementBase("") {}

bool StatementError::decode(const char* line) {
  std::cerr << "[Decode] cannot parse line: " << std::endl;
  std::cerr << std::string(line) << std::endl;
  exit(-1);
}
