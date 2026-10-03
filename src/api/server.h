#pragma once
#include <string>

class App;

int runServer(App& app, int port, const std::string& webDir);
