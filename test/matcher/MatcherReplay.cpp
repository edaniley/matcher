// MatcherReplay: command line harness for the matching engine.
//
//   MatcherReplay [--symbol S] [--stp cancel_newest|cancel_oldest|cancel_both]
//                 [--snapshot none|top|depth] [--max-orders N] [--max-levels N]
//                 [--retained-orders N] [input.jsonl]
//
// Reads JSON Lines commands from the file (or stdin) and writes events / query results /
// errors as JSON Lines to stdout. See JsonCodec.hpp for the command format.
#include <fstream>
#include <iostream>
#include <string>
#include <boost/json/src.hpp>
#include "Replay.hpp"

using namespace hw::matcher;

namespace {

size_t count(const std::string & text) {
  size_t pos = 0;
  unsigned long long v = 0;
  try {
    v = std::stoull(text, &pos);
  }
  catch (const std::exception &) {
    pos = 0;
  }
  if (pos == 0 || pos != text.size() || text[0] == '-') throw json::ParseError("invalid count '" + text + "'");
  return static_cast<size_t>(v);
}

int usage(const char * prog) {
  std::cerr << "usage: " << prog
            << " [--symbol S] [--stp cancel_newest|cancel_oldest|cancel_both]"
               " [--snapshot none|top|depth] [--max-orders N] [--max-levels N]"
               " [--retained-orders N] [input.jsonl]\n";
  return 2;
}

}

int main(int argc, char ** argv) {
  json::ReplayOptions options;
  std::string inputPath;

  try {
    for (int i = 1; i < argc; ++i) {
      const std::string arg = argv[i];
      auto value = [&]() -> std::string {
        if (i + 1 >= argc) throw json::ParseError("missing value for " + arg);
        return argv[++i];
      };
      if      (arg == "--symbol")   options.engine.symbol = value();
      else if (arg == "--stp")      options.engine.selfTradePolicy = json::parseStp(value());
      else if (arg == "--snapshot") options.snapshot = json::parseSnapshot(value());
      else if (arg == "--max-orders")      options.engine.maxOrders = count(value());
      else if (arg == "--max-levels")      options.engine.maxLevels = count(value());
      else if (arg == "--retained-orders") options.engine.retainedOrders = count(value());
      else if (arg == "-h" || arg == "--help") return usage(argv[0]);
      else if (!arg.empty() && arg[0] == '-') throw json::ParseError("unknown option " + arg);
      else if (inputPath.empty()) inputPath = arg;
      else throw json::ParseError("more than one input file given");
    }
  }
  catch (const json::ParseError & e) {
    std::cerr << e.what() << '\n';
    return usage(argv[0]);
  }

  std::ifstream file;
  if (!inputPath.empty()) {
    file.open(inputPath);
    if (!file) {
      std::cerr << "cannot open " << inputPath << '\n';
      return 1;
    }
  }

  std::ios::sync_with_stdio(false);
  json::Replay replay(options, std::cout);
  const json::ReplayStats stats = replay.run(inputPath.empty() ? std::cin : file);
  std::cout.flush();
  std::cerr << "lines=" << stats.lines << " commands=" << stats.commands << " errors=" << stats.errors << '\n';
  return 0;
}
