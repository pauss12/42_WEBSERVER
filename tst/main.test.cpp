#include "Configuration.hpp"
#include <exception>
#include <iostream>
// This tester takes exactly one argument: a path to a configuration file.
// If the evaluator wishes to test some other config files, they can simply
// provide the path to that specific file as the argument.
//
// A Configuration object is instantiated, into which the configuration file is
// parsed. Then the configuration object's print() method is called to dump its
// contents. This main function does nothing else.
int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "Usage: " << argv[0] << " <config_file_path>" << std::endl;
    return 1;
  }

  try {
    Configuration config;
    Configuration::parse(argv[1], config);
    config.print();
  } catch (std::exception &e) {
    std::cerr << "Fatal Error: " << e.what() << std::endl;
    return 1;
  }

  return 0;
}
