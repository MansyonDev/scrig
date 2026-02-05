#include "config.hpp"
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace scrig::config {

static const char* kDefault = R"([node]
address = "127.0.0.1:3003"

[miner]
public = "<your public wallet address>"

[threads]
count = 1

[randomx]
full_dataset = true
)";

static std::string read_all(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if(!f) return {};
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

static void write_all(const std::string& path, const std::string& s) {
  std::ofstream f(path, std::ios::binary);
  if(!f) throw std::runtime_error("cannot write config");
  f.write(s.data(), (std::streamsize)s.size());
}

static std::string trim(std::string s) {
  while(!s.empty() && (s.back()=='\r' || s.back()=='\n' || s.back()==' ' || s.back()=='\t')) s.pop_back();
  size_t i=0;
  while(i<s.size() && (s[i]==' ' || s[i]=='\t')) i++;
  return s.substr(i);
}

static std::string unquote(std::string s) {
  s = trim(s);
  if(s.size()>=2 && s.front()=='"' && s.back()=='"') return s.substr(1, s.size()-2);
  return s;
}

Settings load_or_create(const std::string& path) {
  auto txt = read_all(path);
  if(txt.empty()) {
    write_all(path, kDefault);
    throw std::runtime_error("Created miner.toml, set miner.public then rerun");
  }

  Settings out;
  out.node_address = "127.0.0.1:3003";
  out.miner_public_base36 = "";
  out.threads_count = 1;
  out.full_dataset = true;

  std::string section;
  std::istringstream ss(txt);
  std::string line;
  while(std::getline(ss, line)) {
    line = trim(line);
    if(line.empty()) continue;
    if(line[0]=='[') {
      auto p = line.find(']');
      if(p!=std::string::npos) section = line.substr(1, p-1);
      continue;
    }
    auto eq = line.find('=');
    if(eq==std::string::npos) continue;
    auto key = trim(line.substr(0, eq));
    auto val = trim(line.substr(eq+1));

    if(section=="node" && key=="address") out.node_address = unquote(val);
    if(section=="miner" && key=="public") out.miner_public_base36 = unquote(val);
    if(section=="threads" && key=="count") out.threads_count = std::stoi(val);
    if(section=="randomx" && key=="full_dataset") out.full_dataset = (val=="true" || val=="1");
  }

  if(out.miner_public_base36.empty() || out.miner_public_base36.find("<")!=std::string::npos) {
    throw std::runtime_error("Set miner.public in miner.toml then rerun");
  }

  return out;
}

}