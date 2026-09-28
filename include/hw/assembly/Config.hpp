#pragma once

#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/foreach.hpp>
#include <hw/utility/Text.hpp>

namespace hw::assembly {

namespace pt = boost::property_tree;

class Config {
public:
  using ParentType = pt::ptree;

  Config(const char *filename = nullptr) {
    if (filename) {
      pt::read_json(filename, root);

      if (auto ethers = root.get_child_optional("ethers"); ethers) {
        BOOST_FOREACH(pt::ptree::value_type &v, ethers.value()) {
          ethers_[v.first] = v. second.data();
        }
      }
    }
  }

  const std::string & getEther(const std::string & name) const {
    auto it = ethers_.find(name);
    if (it == ethers_. end())
      it = ethers_.find("default");
    if (it == ethers_.end())
      throw (std::invalid_argument(std::string("Cannot find ether setting for ") + name));
    return it->second;
  }

  template <typename Type>
  Type getConfig(const std::string & object, const std::string & attribute, const std::string & defval) const {
    try {
      return utility::fromString<Type>(root.get_child(object).get<std::string>(attribute));
    }
    catch (... ) {
      return utility::fromString<Type>(defval);
    }
  }

  void setAttribute(const std::string & object, const std::string & attribute, const std::string & val) {
    attributes_[object][attribute] = val;
  }

  template <typename Type>
  Type getAttribute(const std::string & object, const std::string & attribute,  const std::string & defval) const {
    if (auto oit = attributes_.find(object); oit != attributes_.end()) {
      if (auto ait = oit->second.find(attribute); ait != oit->second.end()) {
        return utility::fromString<Type>(ait->second);
      }
    }
    return utility::fromString<Type>(defval);
  }

  pt::ptree getChild (const std::string & child, pt::ptree parent = pt::ptree{}) const {
    if (parent.empty())
      return root.get_child(child);
    else
      return parent.get_child(child);
  }

  pt::ptree root;

private:
  std::map<std::string, std::string>  ethers_;
  std::map<std::string, std::map<std::string, std::string>> attributes_;
};

}
