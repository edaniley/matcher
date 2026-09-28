#pragma once

#include <string>

#include <hw/assembly/Config.hpp>

namespace hw::assembly {

struct Context {
  Context(const std::string name, const char *cfgfile = nullptr)
  : appname(name), config(cfgfile) { }

  const std::string appname;
  Config config;

  const std::string & getEther(const std:: string & name) const {
    return config.getEther(name);
  }

  template <typename Type>
  Type getConfig(const std::string & object, const std::string & attribute, const std::string & defval) const {
    return config.getConfig<Type>(object, attribute, defval);
  }

  void setAttribute(const std::string & object, const std::string & attribute, const std::string & val) {
    config.setAttribute(object, attribute, val);
  }

  template <typename Type>
  Type getAttribute(const std::string & object, const std::string & attribute, const std::string & defval) const {
    return config.getAttribute<Type>(object, attribute, defval);
  }

  Config::ParentType getChild(const std::string & name) const {
    return config.getChild(name);
  }
};

}
