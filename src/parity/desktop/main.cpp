#include "parity/app/application.h"
#include <WebView2.h>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <shellapi.h>
#include <shlwapi.h>
#include <thread>
#include <windows.h>
#include <wrl.h>
using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
namespace p = lexica::parity;
namespace fs = std::filesystem;
namespace {
constexpr UINT kReply = WM_APP + 1;
constexpr wchar_t kOrigin[] = L"https://lexica.local/";
std::wstring wide(const std::string &s) {
  if (s.empty())
    return {};
  const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                                    static_cast<int>(s.size()), nullptr, 0);
  if (!n)
    throw p::Error("BAD_REQUEST", "Invalid UTF-8");
  std::wstring w(n, 0);
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                      static_cast<int>(s.size()), w.data(), n);
  return w;
}
std::string utf8(const std::wstring &s) {
  if (s.empty())
    return {};
  const int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(),
                                    static_cast<int>(s.size()), nullptr, 0,
                                    nullptr, nullptr);
  if (!n)
    throw p::Error("BAD_REQUEST", "Invalid UTF-16");
  std::string a(n, 0);
  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(),
                      static_cast<int>(s.size()), a.data(), n, nullptr,
                      nullptr);
  return a;
}
bool trusted(const std::wstring &uri) { return uri.rfind(kOrigin, 0) == 0; }
bool external(const std::wstring &uri) {
  return uri.rfind(L"https://", 0) == 0 || uri.rfind(L"http://", 0) == 0;
}
void require(HRESULT result) {
  if (FAILED(result))
    throw p::Error("WEBVIEW_ERROR",
                   "WebView2 operation failed: " +
                       std::to_string(static_cast<unsigned long>(result)));
}
struct Host {
  HWND window = nullptr;
  fs::path data = p::defaultDataDirectory(), ui, report;
  bool smoke = false, finished = false;
  int exitCode = 0;
  p::Json diagnostics = p::Json::array();
  ComPtr<ICoreWebView2Environment> environment;
  ComPtr<ICoreWebView2Controller> controller;
  ComPtr<ICoreWebView2> webview;
  std::mutex mutex;
  std::condition_variable ready;
  std::deque<p::Json> requests, replies;
  bool stopping = false;
  std::vector<std::thread> workers;
  std::unique_ptr<p::Application> app;
  void writeReport(p::Json value) {
    if (report.empty())
      return;
    fs::create_directories(report.parent_path());
    std::ofstream out(report, std::ios::binary);
    out << value.dump(2);
  }
  void fail(const std::string &message) {
    exitCode = 1;
    finished = true;
    writeReport(
        {{"ok", false}, {"error", message}, {"diagnostics", diagnostics}});
    if (!smoke)
      MessageBoxW(window, wide(message).c_str(), L"Lexica", MB_ICONERROR);
    if (window)
      DestroyWindow(window);
    else
      PostQuitMessage(1);
  }
  void send(p::Json value) {
    std::lock_guard<std::mutex> lock(mutex);
    if (stopping)
      return;
    replies.push_back(std::move(value));
    PostMessageW(window, kReply, 0, 0);
  }
  void startWorker() {
    app = std::make_unique<p::Application>(
        data / L"lexica.db", nullptr,
        [this](const std::string &name, const p::Json &payload) {
          send(
              {{"protocol_version", 1}, {"event", name}, {"payload", payload}});
        });
    for (int i = 0; i < 4; ++i)
      workers.emplace_back([this] {
        for (;;) {
          p::Json request;
          {
            std::unique_lock<std::mutex> lock(mutex);
            ready.wait(lock, [this] { return stopping || !requests.empty(); });
            if (stopping)
              break;
            request = std::move(requests.front());
            requests.pop_front();
          }
          send(app->dispatch(request));
        }
      });
  }
  void stop() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      stopping = true;
      requests.clear();
      replies.clear();
    }
    ready.notify_all();
    for (auto &worker : workers)
      if (worker.joinable())
        worker.join();
    workers.clear();
    if (controller)
      controller->Close();
    webview.Reset();
    controller.Reset();
    environment.Reset();
    app.reset();
  }
  ~Host() { stop(); }
  void smokeResult(const p::Json &result) {
    if (finished)
      return;
    finished = true;
    exitCode = result.value("ok", false) ? 0 : 1;
    writeReport(result);
    if (!report.empty()) {
      const auto screenshot = report.parent_path() / L"desktop.png";
      ComPtr<IStream> stream;
      if (SUCCEEDED(SHCreateStreamOnFileEx(
              screenshot.c_str(),
              STGM_CREATE | STGM_WRITE | STGM_SHARE_EXCLUSIVE,
              FILE_ATTRIBUTE_NORMAL, TRUE, nullptr, &stream))) {
        const auto hr = webview->CapturePreview(
            COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG, stream.Get(),
            Callback<ICoreWebView2CapturePreviewCompletedHandler>(
                [this, stream](HRESULT) -> HRESULT {
                  PostMessageW(window, WM_CLOSE, 0, 0);
                  return S_OK;
                })
                .Get());
        if (SUCCEEDED(hr))
          return;
      }
    }
    PostMessageW(window, WM_CLOSE, 0, 0);
  }
  void configure() {
    ComPtr<ICoreWebView2Settings> settings;
    require(webview->get_Settings(&settings));
    settings->put_AreDefaultScriptDialogsEnabled(TRUE);
    EventRegistrationToken token{};
    require(webview->add_NavigationStarting(
        Callback<ICoreWebView2NavigationStartingEventHandler>(
            [this](ICoreWebView2 *,
                   ICoreWebView2NavigationStartingEventArgs *args) -> HRESULT {
              LPWSTR raw = nullptr;
              if (SUCCEEDED(args->get_Uri(&raw))) {
                const std::wstring uri = raw;
                CoTaskMemFree(raw);
                if (!trusted(uri))
                  args->put_Cancel(TRUE);
                if (smoke)
                  diagnostics.push_back({{"navigation", utf8(uri)}});
              }
              return S_OK;
            })
            .Get(),
        &token));
    require(webview->add_NewWindowRequested(
        Callback<ICoreWebView2NewWindowRequestedEventHandler>(
            [this](ICoreWebView2 *,
                   ICoreWebView2NewWindowRequestedEventArgs *args) -> HRESULT {
              args->put_Handled(TRUE);
              LPWSTR raw = nullptr;
              if (SUCCEEDED(args->get_Uri(&raw))) {
                const std::wstring uri = raw;
                CoTaskMemFree(raw);
                if (external(uri))
                  ShellExecuteW(window, L"open", uri.c_str(), nullptr, nullptr,
                                SW_SHOWNORMAL);
              }
              return S_OK;
            })
            .Get(),
        &token));
    require(webview->add_WebMessageReceived(
        Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [this](ICoreWebView2 *,
                   ICoreWebView2WebMessageReceivedEventArgs *args) -> HRESULT {
              try {
                LPWSTR rawSource = nullptr;
                require(args->get_Source(&rawSource));
                const std::wstring source = rawSource;
                CoTaskMemFree(rawSource);
                LPWSTR rawTop = nullptr;
                require(webview->get_Source(&rawTop));
                const std::wstring top = rawTop;
                CoTaskMemFree(rawTop);
                if (!trusted(source) || !trusted(top))
                  return S_OK;
                LPWSTR raw = nullptr;
                require(args->get_WebMessageAsJson(&raw));
                const auto input = utf8(raw);
                CoTaskMemFree(raw);
                if (input.size() > 1024 * 1024)
                  return S_OK;
                auto request = p::Json::parse(input, nullptr, false);
                if (smoke && request.is_object() &&
                    request.value("kind", std::string{}) == "phase0-smoke") {
                  smokeResult(request.at("result"));
                  return S_OK;
                }
                bool overflow = false;
                {
                  std::lock_guard<std::mutex> lock(mutex);
                  if (stopping)
                    return S_OK;
                  overflow = requests.size() >= 128;
                  if (!overflow)
                    requests.push_back(request);
                }
                if (overflow) {
                  send({{"protocol_version", 1},
                        {"request_id",
                         request.is_object()
                             ? request.value("request_id", p::Json(nullptr))
                             : p::Json(nullptr)},
                        {"ok", false},
                        {"error",
                         {{"code", "BUSY"},
                          {"message", "Command queue is full"}}}});
                  return S_OK;
                }
                ready.notify_one();
              } catch (const std::exception &e) {
                if (smoke)
                  fail(e.what());
              }
              return S_OK;
            })
            .Get(),
        &token));
    // One provider handles both SPA routes and assets, including Unicode paths.
    // Never hand application requests to DNS or an arbitrary filesystem path.
    require(webview->AddWebResourceRequestedFilter(
        L"https://lexica.local/*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL));
    require(webview->add_WebResourceRequested(
        Callback<ICoreWebView2WebResourceRequestedEventHandler>(
            [this](
                ICoreWebView2 *,
                ICoreWebView2WebResourceRequestedEventArgs *args) -> HRESULT {
              ComPtr<ICoreWebView2WebResourceRequest> request;
              args->get_Request(&request);
              LPWSTR raw = nullptr;
              request->get_Uri(&raw);
              std::wstring uri = raw;
              CoTaskMemFree(raw);
              if (!trusted(uri))
                return S_OK;
              auto route = uri.substr(std::size(kOrigin) - 1);
              route = L"/" + route.substr(0, route.find_first_of(L"?#"));
              const bool document = route == L"/" || route == L"/library" ||
                                    route == L"/review" || route == L"/stats" ||
                                    route == L"/settings";
              auto relative = document ? fs::path(L"index.html")
                                       : fs::path(route.substr(1));
              bool valid = route.find_first_of(L"%\\:") == std::wstring::npos &&
                           !relative.is_absolute();
              for (const auto &part : relative)
                if (part == L".." || part == L".")
                  valid = false;
              fs::path target;
              try {
                const auto root = fs::canonical(ui);
                target = fs::weakly_canonical(root / relative);
                auto a = root.begin(), b = target.begin();
                for (; a != root.end() && b != target.end() && *a == *b;
                     ++a, ++b) {
                }
                if (a != root.end() || b == target.end())
                  valid = false;
              } catch (...) {
                valid = false;
              }
              const bool exists = valid && fs::is_regular_file(target);
              std::ifstream file(exists ? target : fs::path{},
                                 std::ios::binary);
              const std::string bytes(std::istreambuf_iterator<char>(file), {});
              if (smoke)
                diagnostics.push_back({{"resource", utf8(uri)},
                                       {"route", utf8(route)},
                                       {"bytes", bytes.size()}});
              ComPtr<IStream> stream;
              stream.Attach(SHCreateMemStream(
                  reinterpret_cast<const BYTE *>(bytes.data()),
                  static_cast<UINT>(bytes.size())));
              ComPtr<ICoreWebView2WebResourceResponse> response;
              std::ifstream policyFile(ui / L"csp.txt", std::ios::binary);
              const std::string policy(
                  std::istreambuf_iterator<char>(policyFile), {});
              const auto extension = target.extension().wstring();
              const std::wstring mime =
                  document ? L"text/html; charset=utf-8"
                  : extension == L".js"
                      ? L"application/javascript; charset=utf-8"
                  : extension == L".css"   ? L"text/css; charset=utf-8"
                  : extension == L".woff2" ? L"font/woff2"
                  : extension == L".svg"   ? L"image/svg+xml"
                  : extension == L".png"   ? L"image/png"
                  : extension == L".json"  ? L"application/json"
                                           : L"application/octet-stream";
              const auto headers = L"Content-Type: " + mime +
                                   L"\r\nX-Content-Type-Options: "
                                   L"nosniff\r\nContent-Security-Policy: " +
                                   wide(policy) + L"\r\n";
              if (SUCCEEDED(environment->CreateWebResourceResponse(
                      stream.Get(), exists ? 200 : 404,
                      exists ? L"OK" : L"Not Found", headers.c_str(),
                      &response)))
                args->put_Response(response.Get());
              return S_OK;
            })
            .Get(),
        &token));
    if (smoke) {
      webview->AddScriptToExecuteOnDocumentCreated(
          L"window.__LEXICA_SMOKE__=true;", nullptr);
      require(webview->add_NavigationCompleted(
          Callback<ICoreWebView2NavigationCompletedEventHandler>(
              [this](
                  ICoreWebView2 *,
                  ICoreWebView2NavigationCompletedEventArgs *args) -> HRESULT {
                BOOL success = FALSE;
                args->get_IsSuccess(&success);
                if (!success) {
                  COREWEBVIEW2_WEB_ERROR_STATUS error{};
                  args->get_WebErrorStatus(&error);
                  ComPtr<ICoreWebView2NavigationCompletedEventArgs2> more;
                  int http = 0;
                  if (SUCCEEDED(args->QueryInterface(IID_PPV_ARGS(&more))))
                    more->get_HttpStatusCode(&http);
                  fail("Navigation failed; WebView2 status=" +
                       std::to_string(static_cast<int>(error)) +
                       " HTTP=" + std::to_string(http));
                  return S_OK;
                }
                const wchar_t *script =
                    LR"JS((()=>{let n=0;const timer=setInterval(async()=>{if(++n>100){clearInterval(timer);window.chrome.webview.postMessage({kind:'phase0-smoke',result:{ok:false,error:'UI transport did not initialize'}});return;}if(!window.__LEXICA_INVOKE__)return;clearInterval(timer);try{const invoke=window.__LEXICA_INVOKE__;const info=await invoke('app_info');const suggestions=await invoke('search_suggest',{query:'met',limit:8});const lookup=await invoke('lookup_word',{word:'meticulous'});let unavailable='';try{await invoke('save_word_note',{wordId:lookup.outcome.entry.id,bookmarked:true,note:'smoke'});}catch(e){unavailable=e.code;}setTimeout(()=>window.chrome.webview.postMessage({kind:'phase0-smoke',result:{ok:info.fts_ok&&info.schema_version===5&&lookup.outcome.type==='hit'&&suggestions.length>0&&unavailable==='NOT_IMPLEMENTED',info,suggestions,lookup,unavailable,title:document.title,ui_rendered:document.body.textContent.includes('meticulous')}}),500);}catch(e){window.chrome.webview.postMessage({kind:'phase0-smoke',result:{ok:false,error:String(e)}});}},100);})())JS";
                webview->ExecuteScript(script, nullptr);
                return S_OK;
              })
              .Get(),
          &token));
    }
    RECT bounds{};
    GetClientRect(window, &bounds);
    controller->put_Bounds(bounds);
    controller->put_IsVisible(TRUE);
    require(webview->Navigate(smoke ? L"https://lexica.local/?word=meticulous"
                                    : kOrigin));
  }
  void init() {
    if (!fs::exists(ui / L"index.html") || !fs::exists(ui / L"csp.txt"))
      throw p::Error("RESOURCE_ERROR",
                     "Build the frontend and package its ui folder first");
    startWorker();
    require(CreateCoreWebView2EnvironmentWithOptions(
        nullptr, (data / L"webview-profile").c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this](HRESULT result, ICoreWebView2Environment *env) -> HRESULT {
              if (FAILED(result) || !env) {
                fail("WebView2 Runtime is unavailable");
                return S_OK;
              }
              environment = env;
              const auto hr = environment->CreateCoreWebView2Controller(
                  window,
                  Callback<
                      ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                      [this](HRESULT result,
                             ICoreWebView2Controller *control) -> HRESULT {
                        try {
                          require(result);
                          controller = control;
                          require(controller->get_CoreWebView2(&webview));
                          configure();
                        } catch (const std::exception &e) {
                          fail(e.what());
                        }
                        return S_OK;
                      })
                      .Get());
              if (FAILED(hr))
                fail("Cannot create WebView2 controller");
              return S_OK;
            })
            .Get()));
  }
};
LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM w, LPARAM l) {
  auto *host =
      reinterpret_cast<Host *>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    host = static_cast<Host *>(
        reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
    host->window = window;
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(host));
  }
  if (!host)
    return DefWindowProcW(window, message, w, l);
  switch (message) {
  case WM_SIZE:
    if (host->controller) {
      RECT r{};
      GetClientRect(window, &r);
      host->controller->put_Bounds(r);
    }
    return 0;
  case WM_GETMINMAXINFO: {
    auto *info = reinterpret_cast<MINMAXINFO *>(l);
    const auto dpi = GetDpiForWindow(window);
    RECT r{0, 0, MulDiv(480, dpi, 96), MulDiv(400, dpi, 96)};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    info->ptMinTrackSize = {r.right - r.left, r.bottom - r.top};
    return 0;
  }
  case kReply: {
    std::deque<p::Json> replies;
    {
      std::lock_guard<std::mutex> lock(host->mutex);
      replies.swap(host->replies);
    }
    for (const auto &reply : replies)
      if (host->webview)
        host->webview->PostWebMessageAsJson(wide(reply.dump()).c_str());
    return 0;
  }
  case WM_TIMER:
    if (host->smoke && !host->finished)
      host->fail("Desktop smoke timeout");
    return 0;
  case WM_DESTROY:
    host->stop();
    PostQuitMessage(host->exitCode);
    return 0;
  default:
    return DefWindowProcW(window, message, w, l);
  }
}
} // namespace
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int show) {
  Host host;
  try {
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv)
      return 1;
    wchar_t exe[32768]{};
    GetModuleFileNameW(nullptr, exe, 32768);
    host.ui = fs::path(exe).parent_path() / L"ui";
    for (int i = 1; i < argc; ++i) {
      std::wstring a = argv[i];
      if (a == L"--data-dir" && i + 1 < argc)
        host.data = argv[++i];
      else if (a == L"--ui-dir" && i + 1 < argc)
        host.ui = fs::absolute(argv[++i]);
      else if (a == L"--smoke-report" && i + 1 < argc) {
        host.report = fs::absolute(argv[++i]);
        host.smoke = true;
      } else {
        LocalFree(argv);
        throw p::Error("BAD_REQUEST", "Invalid desktop argument");
      }
    }
    LocalFree(argv);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    require(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
    WNDCLASSW wc{};
    wc.lpfnWndProc = windowProc;
    wc.hInstance = instance;
    wc.lpszClassName = L"LexicaReproduceParity";
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    RegisterClassW(&wc);
    const auto dpi = GetDpiForSystem();
    RECT r{0, 0, MulDiv(1180, dpi, 96), MulDiv(820, dpi, 96)};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    const auto hwnd =
        CreateWindowExW(0, wc.lpszClassName, L"Lexica", WS_OVERLAPPEDWINDOW,
                        CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left,
                        r.bottom - r.top, nullptr, nullptr, instance, &host);
    if (!hwnd)
      throw p::Error("WINDOW_ERROR", "Cannot create window");
    if (!host.smoke)
      ShowWindow(hwnd, show);
    else
      SetTimer(hwnd, 1, 30000, nullptr);
    host.init();
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    host.stop();
    CoUninitialize();
    return host.exitCode;
  } catch (const std::exception &e) {
    host.fail(e.what());
    return 1;
  }
}
