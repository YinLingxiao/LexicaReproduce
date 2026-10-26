#include "parity/api/router.h"
#include "parity/contracts/cancel.h"
#include "parity/contracts/catalog.h"
#include <cmath>
namespace lexica::parity {
namespace {
const Json models = Json::parse(kModels);
void validate(const Json &value, const std::string &type,
              const std::string &field) {
  if (!type.empty() && type.back() == '?') {
    if (!value.is_null())
      validate(value, type.substr(0, type.size() - 1), field);
    return;
  }
  if (type.size() > 2 && type.substr(type.size() - 2) == "[]") {
    if (!value.is_array())
      throw Error("BAD_REQUEST", field + " must be an array");
    for (const auto &item : value)
      validate(item, type.substr(0, type.size() - 2), field + "[]");
    return;
  }
  bool valid = false;
  if (type == "string")
    valid = value.is_string();
  else if (type == "bool")
    valid = value.is_boolean();
  else if (type == "int")
    valid = value.is_number_integer() &&
            value.get<double>() >= -9007199254740991.0 &&
            value.get<double>() <= 9007199254740991.0;
  else if (type == "number")
    valid = value.is_number() && std::isfinite(value.get<double>());
  else if (type == "void")
    valid = value.is_null();
  else if (models["enums"].contains(type)) {
    for (const auto &choice : models["enums"][type])
      if (choice == value)
        valid = true;
  } else if (models["models"].contains(type)) {
    if (!value.is_object())
      throw Error("BAD_REQUEST", field + " must be an object");
    for (const auto &f : models["models"][type].items()) {
      if (!value.contains(f.key()))
        throw Error("BAD_REQUEST", field + " missing " + f.key());
      validate(value.at(f.key()), f.value(), field + "." + f.key());
    }
    valid = true;
  } else if (models["unions"].contains(type)) {
    if (!value.is_object() || !value.contains("type") ||
        !value["type"].is_string())
      throw Error("BAD_REQUEST", field + " requires a discriminator");
    const auto tag = value.at("type").get<std::string>();
    if (models["unions"][type].contains(tag)) {
      for (const auto &f : models["unions"][type][tag].items()) {
        if (!value.contains(f.key()))
          throw Error("BAD_REQUEST", field + " missing " + f.key());
        validate(value.at(f.key()), f.value(), field + "." + f.key());
      }
      valid = true;
    }
  }
  if (!valid)
    throw Error("BAD_REQUEST", field + " does not match " + type);
}
} // namespace
Router::Router() : catalog_(Json::parse(kCommands)) {}
void Router::bind(const std::string &owner, const std::string &command,
                  Handler handler, const std::string &implementation) {
  for (const auto &c : catalog_.at("commands"))
    if (c.at("name") == command) {
      if (c.at("owner") != owner)
        throw Error("CONTRACT_ERROR", "Command owner mismatch: " + command);
      if (!handlers_.emplace(command, std::move(handler)).second)
        throw Error("CONTRACT_ERROR", "Duplicate command: " + command);
      status_[command] = implementation;
      return;
    }
  throw Error("CONTRACT_ERROR", "Unregistered contract command: " + command);
}
Json Router::invoke(const std::string &command, Json args,
                    Context &context) const {
  struct CancelBind {
    const Cancellation *previous;
    explicit CancelBind(const Cancellation *current)
        : previous(currentCancellation()) {
      setCurrentCancellation(current);
    }
    ~CancelBind() { setCurrentCancellation(previous); }
  } bind(context.cancellation);
  throwIfCancelled(context.cancellation);
  if (command == "system_capabilities")
    return capabilities();
  const Json *spec = nullptr;
  for (const auto &c : catalog_.at("commands"))
    if (c.at("name") == command) {
      spec = &c;
      break;
    }
  if (!spec)
    throw Error("UNKNOWN_COMMAND", "Unknown command");
  if (!args.is_object())
    throw Error("BAD_REQUEST", "args must be an object");
  for (const auto &f : args.items())
    if (!spec->at("args").contains(f.key()))
      throw Error("BAD_REQUEST", "Unexpected argument: " + f.key());
  for (const auto &f : spec->at("args").items()) {
    const auto type = f.value().get<std::string>();
    if (!args.contains(f.key()) || args[f.key()].is_null()) {
      if (spec->contains("defaults") && spec->at("defaults").contains(f.key()))
        args[f.key()] = spec->at("defaults").at(f.key());
      else if (!args.contains(f.key()) && type.back() == '?')
        args[f.key()] = nullptr;
    }
    if (!args.contains(f.key()))
      throw Error("BAD_REQUEST", "Missing argument: " + f.key());
    validate(args.at(f.key()), type, f.key());
    if (spec->contains("bounds") && spec->at("bounds").contains(f.key())) {
      const auto &b = spec->at("bounds").at(f.key());
      if (args[f.key()] < b[0] || args[f.key()] > b[1])
        throw Error("BAD_REQUEST", "Argument out of range: " + f.key());
    }
  }
  auto handler = handlers_.find(command);
  if (handler == handlers_.end())
    notImplemented(command);
  auto result = handler->second(args, context);
  try {
    validate(result, spec->at("result"), "result");
  } catch (const Error &) {
    throw Error("CONTRACT_ERROR",
                "Handler returned an invalid result: " + command);
  }
  return result;
}
Json Router::dispatch(const Json &request, Context &context) const {
  Json id = nullptr;
  try {
    if (!request.is_object())
      throw Error("BAD_REQUEST", "Request must be an object");
    if (request.contains("request_id") && request["request_id"].is_string() &&
        request["request_id"].get_ref<const std::string &>().size() <= 128)
      id = request["request_id"];
    else
      throw Error("BAD_REQUEST", "request_id must be a string up to 128 bytes");
    if (!request.contains("protocol_version") ||
        !request["protocol_version"].is_number_integer() ||
        request["protocol_version"] != 1)
      throw Error("PROTOCOL_MISMATCH", "Expected protocol_version 1");
    if (!request.contains("command") || !request["command"].is_string())
      throw Error("BAD_REQUEST", "command must be a string");
    auto perCall = context;
    perCall.request_id = id.get<std::string>();
    auto result = invoke(request["command"],
                         request.value("args", Json::object()), perCall);
    return Json{{"protocol_version", 1},
                {"request_id", id},
                {"ok", true},
                {"result", std::move(result)}};
  } catch (const Error &e) {
    return Json{{"protocol_version", 1},
                {"request_id", id},
                {"ok", false},
                {"error", {{"code", e.code}, {"message", e.what()}}}};
  } catch (const Json::exception &) {
    return Json{{"protocol_version", 1},
                {"request_id", id},
                {"ok", false},
                {"error",
                 {{"code", "BAD_REQUEST"}, {"message", "Invalid JSON field"}}}};
  } catch (const std::exception &) {
    return Json{
        {"protocol_version", 1},
        {"request_id", id},
        {"ok", false},
        {"error", {{"code", "INTERNAL_ERROR"}, {"message", "Command failed"}}}};
  }
}
Json Router::capabilities() const {
  auto result = catalog_;
  for (auto &c : result["commands"]) {
    const auto name = c["name"].get<std::string>();
    c["available"] = handlers_.count(name) != 0;
    c["implementation"] =
        status_.count(name) ? status_.at(name) : "not_implemented";
  }
  result["foundation_phase"] = 0;
  result["automatic_dictionary_setup"] =
      handlers_.count("ensure_dictionary") != 0;
  return result;
}
} // namespace lexica::parity
