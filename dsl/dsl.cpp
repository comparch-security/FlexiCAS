#include "dsl/statement.hpp"
#include "dsl/type_description.hpp"


int main(int argc, char* argv[]) {
  if(argc != 3) {
    std::cerr << "Usage: dsl config_file cache_name" << std::endl;
    std::cerr << "  Generate a cache system according to `config_file' and write it to `cache_name.hpp' and `cache_name.cpp'." << std::endl;
    return 1;
  }

  std::ifstream cfg_file; cfg_file.open(std::string(argv[1]));
  std::ofstream cache_hpp; cache_hpp.open(std::string(argv[2])+".hpp");
  std::ofstream cache_cpp; cache_cpp.open(std::string(argv[2])+".cpp");
  char line[2048];

  while(cfg_file.good()) {
    cfg_file.getline(line, 2048);
    for(auto decoder:codegendb.decoders)
      if(decoder->decode(line))
        break;
  }
  
  codegendb.emit_hpp(cache_hpp);
  codegendb.emit_cpp(cache_cpp);
  cfg_file.close();
  cache_hpp.close();
  cache_cpp.close();

  return 0;
}
