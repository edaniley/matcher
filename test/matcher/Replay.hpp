#pragma once
//
// Replay: drives a MatchingEngine from a JSON Lines command stream and writes JSON Lines results.
// - Blank lines and lines starting with '#' are ignored (comments in scenario files).
// - Malformed commands produce an {"error":...,"line":N} record; processing continues.
// - Output depends only on the input, so replaying the same file always yields identical bytes.
//
#include <istream>
#include <memory>
#include <ostream>
#include <string>
#include <hw/matcher/MatchingEngine.hpp>
#include "JsonCodec.hpp"

namespace hw::matcher::json {

struct ReplayOptions {
  EngineConfig engine{.symbol = "BTC", .selfTradePolicy = SelfTradePolicy::CancelNewest};
  SnapshotMode snapshot = SnapshotMode::None;
};

struct ReplayStats {
  size_t lines    = 0;
  size_t commands = 0;
  size_t errors   = 0;
};

class Replay {
public:
  Replay(ReplayOptions options, std::ostream & out) : _options(std::move(options)), _out(out) {}

  ReplayStats run(std::istream & in) {
    std::string line;
    while (std::getline(in, line)) {
      feed(line);
    }
    return _stats;
  }

  // processes one input line
  void feed(std::string_view line) {
    ++_stats.lines;
    const size_t first = line.find_first_not_of(" \t\r");
    if (first == std::string_view::npos || line[first] == '#') return;
    try {
      execute(parseCommand(line));
      ++_stats.commands;
    }
    catch (const ParseError & e) {
      ++_stats.errors;
      write(errorJson(_stats.lines, e.what()));
    }
  }

  const MatchingEngine & engine() { return ensureEngine(); }
  const ReplayStats &    stats() const noexcept { return _stats; }

private:
  void execute(const Command & cmd) {
    std::visit([&](const auto & c) { execute(c); }, cmd);
  }

  void execute(const NewOrder & req)    { apply(req); }
  void execute(const CancelOrder & req) { apply(req); }

  void execute(const TopQuery &)      { write(toJson("query", ensureEngine().top())); }
  void execute(const DepthQuery & q)  { write(toJson("query", ensureEngine().depth(q.levels))); }
  void execute(const StatusQuery & q) { write(toJson(q.id, ensureEngine().status(q.id))); }

  void execute(const ConfigCmd & cfg) {
    if (_engine) throw ParseError("config must precede all other commands");
    if (cfg.symbol)          _options.engine.symbol = *cfg.symbol;
    if (cfg.selfTradePolicy) _options.engine.selfTradePolicy = *cfg.selfTradePolicy;
    if (cfg.snapshot)        _options.snapshot = *cfg.snapshot;
    if (cfg.maxOrders)       _options.engine.maxOrders = *cfg.maxOrders;
    if (cfg.maxLevels)       _options.engine.maxLevels = *cfg.maxLevels;
    if (cfg.retainedOrders)  _options.engine.retainedOrders = *cfg.retainedOrders;
  }

  template <typename Req>
  void apply(const Req & req) {
    ensureEngine().process(req, [&](const Event & e) { write(toJson(e)); });
    switch (_options.snapshot) {
      case SnapshotMode::None:  break;
      case SnapshotMode::Top:   write(toJson("snapshot", _engine->top())); break;
      case SnapshotMode::Depth: write(toJson("snapshot", _engine->depth())); break;
    }
  }

  MatchingEngine & ensureEngine() {
    if (!_engine) _engine = std::make_unique<MatchingEngine>(_options.engine);
    return *_engine;
  }

  void write(const bj::object & obj) { _out << bj::serialize(obj) << '\n'; }

  ReplayOptions                   _options;
  std::ostream &                  _out;
  std::unique_ptr<MatchingEngine> _engine;   // created lazily so leading config lines apply
  ReplayStats                     _stats;
};

}
