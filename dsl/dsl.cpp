#include "dsl/statement.hpp"
#include "dsl/type_description.hpp"


int main(int argc, char* argv[]) {
  // parsing the argument
  std::string cfg_fn;
  std::string cache_fn; // default name
  for(int i=1; i<argc; i++) {
    std::string arg(argv[i]);
    if(arg == "--help" || arg == "-h") {
      std::cout << "Usage: dsl [-h | --help] [-d | --debug] <config> [<output>]" << std::endl;
      std::cout << std::endl;
      std::cout << "Generate a cache system according to <config> and write it to <output>.hpp and" << std::endl;
      std::cout << "<output>.cpp." << std::endl;
      std::cout << std::endl;
      std::cout << "  config     The DSL configuration file." << std::endl;
      std::cout << "  output     The name of the generated cache c++ files." << std::endl;
      std::cout << "             If not provided, the name of the namespace will be used instead." << std::endl;
      std::cout << "             If the namespace if unused, the default name is `cache_top'."     << std::endl;
      std::cout << "  options:" << std::endl;
      std::cout << "    -h | --help    Show this help information." << std::endl;
      std::cout << "    -d | --debug   Show parsing details." << std::endl;
      std::cout << std::endl;
      exit(0);
    } else if(arg == "--debug" || arg == "-d") codegendb.debug = true;
    else if(cfg_fn.empty()) cfg_fn = arg;
    else if(cache_fn.empty()) cache_fn = arg;
    else {
      std::cout << "Unrecognized argument `" << arg << "'." << std::endl;
      exit(1);
    }
  }

  std::ifstream cfg_file;
  std::ofstream cache_hpp;
  std::ofstream cache_cpp;

  if(cfg_fn.empty()) {
    std::cout << "No configuration is provided!" << std::endl;
    exit(1);
  } else
    cfg_file.open(cfg_fn);

  char line[2048];
  while(cfg_file.good()) {
    cfg_file.getline(line, 2048);
    for(auto decoder:codegendb.decoders)
      if(decoder->decode(line))
        break;
  }
  cfg_file.close();

  if(cache_fn.empty()) cache_fn = codegendb.space;
  if(cache_fn.empty()) cache_fn = "cache_top";
  cache_hpp.open(cache_fn+".hpp");
  cache_cpp.open(cache_fn+".cpp");

  codegendb.emit_hpp(cache_hpp);
  codegendb.emit_cpp(cache_cpp, cache_fn+".hpp");
  cache_hpp.close();
  cache_cpp.close();

  return 0;
}
