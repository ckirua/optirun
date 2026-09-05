#include "hybrid_python/runtime_manager.hpp"
#include <iostream>
using namespace hybrid_python;
int main(int argc, char **argv) {
  if (argc != 4) {
    std::cerr << "usage: native_dispatch GIL_PYTHON FREE_PYTHON HANDLERS\n";
    return 2;
  }
  try {
    RuntimeConfig config;
    config.gil = {argv[1], argv[3]};
    config.free_threaded = {argv[2], argv[3]};
    Runtime runtime(std::move(config));
    for (auto name :
         {"echo", "identity", "raise_value_error", "sleep_then_echo"})
      runtime.register_handler(name, {Backend::gil, Backend::free_threaded});
    runtime.start();
    for (auto backend : {Backend::gil, Backend::free_threaded}) {
      const auto info = runtime.worker_info(backend);
      std::cout << info.executable << " version " << info.version[0] << '.'
                << info.version[1] << " gil=" << info.gil_enabled << '\n';
      std::cout << std::get<std::string>(
                       runtime.submit(backend, "identity", {}).get())
                << '\n';
      std::cout
          << std::get<std::string>(
                 runtime.submit(backend, "echo", {std::string("ok")}).get())
          << '\n';
    }
    runtime.shutdown();
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
