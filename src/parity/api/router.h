#pragma once
#include "parity/contracts/ports.h"
#include <map>
namespace lexica::parity {
using Handler = std::function<Json(const Json &, Context &)>;
class Router {
  Json catalog_;
  std::map<std::string, Handler> handlers_;
  std::map<std::string, std::string> status_;

public:
  Router();
  void bind(const std::string &owner, const std::string &command,
            Handler handler, const std::string &implementation = "implemented");
  Json invoke(const std::string &command, Json args, Context &context) const;
  Json dispatch(const Json &request, Context &context) const;
  Json capabilities() const;
};
} // namespace lexica::parity
