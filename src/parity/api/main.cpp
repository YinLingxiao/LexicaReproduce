#include "httplib.h"
#include "parity/app/application.h"
#include <fstream>
#include <iostream>
#include <set>
#include <shellapi.h>
#include <windows.h>
namespace p = lexica::parity;
int main() {
  SetConsoleOutputCP(CP_UTF8);
  try {
    auto dir = p::defaultDataDirectory();
    std::filesystem::path web = std::filesystem::u8path(LEXICA_DEFAULT_UI_DIR);
    bool stdio = false;
    int port = 8766;
    int argc = 0;
    auto rawArgs = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!rawArgs)
      throw p::Error("BAD_REQUEST", "Cannot parse command line");
    std::vector<std::wstring> argv;
    for (int i = 0; i < argc; ++i)
      argv.emplace_back(rawArgs[i]);
    LocalFree(rawArgs);
    for (int i = 1; i < argc; ++i) {
      const std::wstring a = argv[i];
      if (a == L"--stdio")
        stdio = true;
      else if (a == L"--data-dir" && i + 1 < argc)
        dir = std::filesystem::path(argv[++i]);
      else if (a == L"--ui-dir" && i + 1 < argc)
        web = std::filesystem::path(argv[++i]);
      else if (a == L"--port" && i + 1 < argc)
        port = std::stoi(argv[++i]);
      else
        throw p::Error("BAD_REQUEST", "Unknown/incomplete argument");
    }
    if (port < 1 || port > 65535)
      throw p::Error("BAD_REQUEST", "Invalid port");
    p::Application app(dir / L"lexica.db");
    if (stdio) {
      std::string line;
      while (std::getline(std::cin, line)) {
        const auto request = p::Json::parse(line, nullptr, false);
        std::cout << app.dispatch(request).dump() << std::endl;
      }
      return 0;
    }
    httplib::Server server;
    std::ifstream policyFile(web / L"csp.txt", std::ios::binary);
    const std::string policy(std::istreambuf_iterator<char>(policyFile), {});
    server.set_payload_max_length(1024 * 1024);
    server.set_pre_routing_handler(
        [&](const httplib::Request &req, httplib::Response &res) {
          const auto suffix = ":" + std::to_string(port);
          const auto host = req.get_header_value("Host");
          const auto origin = req.get_header_value("Origin");
          const bool allowedHost =
              host == "127.0.0.1" + suffix || host == "localhost" + suffix;
          const bool allowedOrigin = origin.empty() ||
                                     origin == "http://127.0.0.1" + suffix ||
                                     origin == "http://localhost" + suffix ||
                                     origin == "http://127.0.0.1:5173";
          if (!allowedHost || !allowedOrigin) {
            res.status = 403;
            res.set_content("Forbidden", "text/plain");
            return httplib::Server::HandlerResponse::Handled;
          }
          res.set_header("X-Content-Type-Options", "nosniff");
          if (!policy.empty())
            res.set_header("Content-Security-Policy", policy);
          return httplib::Server::HandlerResponse::Unhandled;
        });
    server.Post("/rpc", [&](const auto &req, auto &res) {
      if (req.get_header_value("Content-Type").find("application/json") != 0) {
        res.status = 415;
        res.set_content("Expected application/json", "text/plain");
        return;
      }
      res.set_content(
          app.dispatch(p::Json::parse(req.body, nullptr, false)).dump(),
          "application/json; charset=utf-8");
    });
    if (std::filesystem::exists(web / L"index.html")) {
      server.set_mount_point("/", web.u8string());
      server.set_error_handler([&](const auto &req, auto &res) {
        if (req.method == "GET" && res.status == 404 && req.path != "/rpc" &&
            req.path.find('.') == std::string::npos) {
          std::ifstream in(web / L"index.html", std::ios::binary);
          res.status = 200;
          res.set_content(std::string(std::istreambuf_iterator<char>(in), {}),
                          "text/html; charset=utf-8");
        }
      });
    }
    std::cout << "Lexica parity development server: http://127.0.0.1:" << port
              << "\n"
              << std::flush;
    if (!server.listen("127.0.0.1", port))
      throw p::Error("NETWORK_ERROR", "Cannot bind loopback port");
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
