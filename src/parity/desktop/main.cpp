#include "parity/app/application.h"
#include "parity/app/executor.h"
#include "parity/app/paths.h"
#include <WebView2.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <fstream>
#include <optional>
#include <shellapi.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <thread>
#include <windows.h>
#include <wrl.h>
using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
namespace p = lexica::parity;
namespace fs = std::filesystem;
namespace {
constexpr UINT kReply = WM_APP + 1;
constexpr UINT kStartup = WM_APP + 2;
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
std::wstring webViewRuntimeVersion() {
  const wchar_t *clients[] = {
      L"SOFTWARE\\WOW6432Node\\Microsoft\\EdgeUpdate\\Clients\\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}",
      L"SOFTWARE\\Microsoft\\EdgeUpdate\\Clients\\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}"};
  for (const wchar_t *subkey : clients) {
    for (const HKEY hive : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER}) {
      HKEY key = nullptr;
      if (RegOpenKeyExW(hive, subkey, 0, KEY_READ, &key) != ERROR_SUCCESS)
        continue;
      wchar_t value[64]{};
      DWORD size = sizeof(value);
      DWORD type = 0;
      const auto status = RegQueryValueExW(key, L"pv", nullptr, &type,
                                           reinterpret_cast<BYTE *>(value), &size);
      RegCloseKey(key);
      if (status == ERROR_SUCCESS && type == REG_SZ && value[0] &&
          wcscmp(value, L"0.0.0.0") != 0)
        return value;
    }
  }
  return {};
}
std::optional<std::wstring> pickPath(HWND owner, bool directory) {
  ComPtr<IFileOpenDialog> dialog;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&dialog))))
    return std::nullopt;
  DWORD options = 0;
  dialog->GetOptions(&options);
  dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR |
                     (directory ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST));
  if (!directory) {
    const COMDLG_FILTERSPEC filters[] = {{L"SQLite", L"*.db"}, {L"All files", L"*.*"}};
    dialog->SetFileTypes(2, filters);
  }
  if (FAILED(dialog->Show(owner)))
    return std::nullopt;
  ComPtr<IShellItem> item;
  PWSTR path = nullptr;
  if (FAILED(dialog->GetResult(&item)) ||
      FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
    return std::nullopt;
  std::wstring result = path;
  CoTaskMemFree(path);
  return result;
}
bool writeClipboard(HWND owner, const std::wstring &text) {
  if (!OpenClipboard(owner))
    return false;
  EmptyClipboard();
  const auto bytes = (text.size() + 1) * sizeof(wchar_t);
  auto memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
  if (!memory) {
    CloseClipboard();
    return false;
  }
  std::memcpy(GlobalLock(memory), text.c_str(), bytes);
  GlobalUnlock(memory);
  if (!SetClipboardData(CF_UNICODETEXT, memory)) {
    GlobalFree(memory);
    CloseClipboard();
    return false;
  }
  CloseClipboard();
  return true;
}
struct Host {
  HWND window = nullptr;
  fs::path root, ui, report;
  p::DataLayout layout{};
  bool smoke = false, finished = false;
  bool automaticDictionarySetup = true;
  int exitCode = 0;
  p::Json diagnostics = p::Json::array();
  ComPtr<ICoreWebView2Environment> environment;
  ComPtr<ICoreWebView2Controller> controller;
  ComPtr<ICoreWebView2> webview;
  std::mutex mutex;
  std::deque<p::Json> replies;
  bool stopping = false, stopped = false;
  std::unique_ptr<p::Application> app;
  std::unique_ptr<p::Executor> executor;
  std::thread startupThread;
  std::atomic<bool> startupDone{false};
  p::Cancellation startupStop;
  std::string startupError;
  std::int64_t startupMs = 0;
  bool closePending = false;
  bool interfaceReady = false;
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
    SetWindowTextW(window, L"Lexica · Opening dictionary");
    startupThread = std::thread([this] {
      const auto began = std::chrono::steady_clock::now();
      p::setCurrentCancellation(&startupStop);
      try {
        app = std::make_unique<p::Application>(
            layout.databaseFile, nullptr,
            [this](const std::string &name, const p::Json &payload) {
              send({{"protocol_version", 1}, {"event", name}, {"payload", payload}});
            }, !smoke && automaticDictionarySetup);
      } catch (const std::exception &error) {
        startupError = error.what();
      }
      p::setCurrentCancellation(nullptr);
      startupMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - began).count();
      startupDone.store(true, std::memory_order_release);
      PostMessageW(window, kStartup, 0, 0);
    });
    SetTimer(window, 2, 500, nullptr);
  }
  void finishStartup() {
    if (!startupDone.load(std::memory_order_acquire))
      return;
    if (startupThread.joinable())
      startupThread.join();
    if (closePending) {
      DestroyWindow(window);
      return;
    }
    if (!startupError.empty()) {
      fail(startupError);
      return;
    }
    executor = std::make_unique<p::Executor>(
        [this](const p::Json &request, const p::Cancellation *cancel) {
          return app->dispatch(request, cancel);
        });
    startWebview();
  }
  void paintStartup() {
    PAINTSTRUCT paint{};
    const auto dc = BeginPaint(window, &paint);
    RECT area{};
    GetClientRect(window, &area);
    const auto background = CreateSolidBrush(RGB(247, 244, 238));
    FillRect(dc, &area, background);
    DeleteObject(background);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(73, 67, 61));
    const auto font = CreateFontW(-MulDiv(17, GetDpiForWindow(window), 96), 0, 0, 0,
                                 FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
    const auto previous = SelectObject(dc, font);
    const wchar_t *text = closePending ? L"Finishing startup safely…"
        : startupDone.load(std::memory_order_acquire) ? L"Dictionary ready. Opening the interface…"
                                                     : L"Opening your local dictionary…";
    DrawTextW(dc, text, -1, &area, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, previous);
    DeleteObject(font);
    EndPaint(window, &paint);
  }
  void stop() {
    startupStop.request();
    if (startupThread.joinable())
      startupThread.join();
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (stopped)
        return;
      stopped = true;
      stopping = true;
      replies.clear();
    }
    if (executor)
      executor->shutdown();
    executor.reset();
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
    auto measured = result;
    measured["backend_startup_ms"] = startupMs;
    writeReport(measured);
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
    settings->put_AreHostObjectsAllowed(FALSE);
    settings->put_IsStatusBarEnabled(FALSE);
    ComPtr<ICoreWebView2Controller3> scale;
    if (SUCCEEDED(controller.As(&scale)))
      scale->put_ShouldDetectMonitorScaleChanges(TRUE);
    EventRegistrationToken permission{};
    require(webview->add_PermissionRequested(
        Callback<ICoreWebView2PermissionRequestedEventHandler>(
            [](ICoreWebView2 *,
               ICoreWebView2PermissionRequestedEventArgs *args) -> HRESULT {
              COREWEBVIEW2_PERMISSION_KIND kind{};
              args->get_PermissionKind(&kind);
              LPWSTR raw = nullptr;
              std::wstring uri;
              if (SUCCEEDED(args->get_Uri(&raw)) && raw) {
                uri = raw;
                CoTaskMemFree(raw);
              }
              args->put_State(kind == COREWEBVIEW2_PERMISSION_KIND_CLIPBOARD_READ &&
                                      trusted(uri)
                                  ? COREWEBVIEW2_PERMISSION_STATE_ALLOW
                                  : COREWEBVIEW2_PERMISSION_STATE_DENY);
              return S_OK;
            })
            .Get(),
        &permission));
    EventRegistrationToken token{};
    require(webview->add_NavigationStarting(
        Callback<ICoreWebView2NavigationStartingEventHandler>(
            [this](ICoreWebView2 *,
                   ICoreWebView2NavigationStartingEventArgs *args) -> HRESULT {
              try {
              LPWSTR raw = nullptr;
              if (SUCCEEDED(args->get_Uri(&raw))) {
                const std::wstring uri = raw;
                CoTaskMemFree(raw);
                if (!trusted(uri)) {
                  args->put_Cancel(TRUE);
                  if (external(uri))
                    ShellExecuteW(window, L"open", uri.c_str(), nullptr, nullptr,
                                  SW_SHOWNORMAL);
                }
                if (smoke)
                  diagnostics.push_back({{"navigation", utf8(uri)}});
              }
              return S_OK;
              } catch (const std::exception &) {
                args->put_Cancel(TRUE);
                return S_OK;
              }
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
                if (request.is_object() &&
                    request.value("kind", std::string{}) == "host") {
                  const auto id = request.value("request_id", p::Json(nullptr));
                  const auto action = request.value("action", std::string{});
                  p::Json reply{{"protocol_version", 1},
                                {"request_id", id},
                                {"ok", true},
                                {"result", nullptr}};
                  if (action == "pick-file" || action == "pick-directory") {
                    const auto path = pickPath(window, action == "pick-directory");
                    reply["result"] = {
                        {"path", path ? p::Json(utf8(*path)) : p::Json(nullptr)}};
                  } else if (action == "clipboard-write") {
                    if (!writeClipboard(window, wide(request.value("text", std::string{})))) {
                      reply["ok"] = false;
                      reply.erase("result");
                      reply["error"] = {{"code", "RESOURCE_ERROR"},
                                        {"message", "Clipboard is unavailable"}};
                    }
                  } else {
                    reply["ok"] = false;
                    reply.erase("result");
                    reply["error"] = {{"code", "BAD_REQUEST"},
                                      {"message", "Unknown host action"}};
                  }
                  send(std::move(reply));
                  return S_OK;
                }
                if (smoke && request.is_object() &&
                    request.value("kind", std::string{}) == "phase0-smoke") {
                  smokeResult(request.at("result"));
                  return S_OK;
                }
                if (!executor)
                  return S_OK;
                executor->submit(std::move(request),
                                 [this](p::Json reply) { send(std::move(reply)); });
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
              try {
              ComPtr<ICoreWebView2WebResourceRequest> request;
              args->get_Request(&request);
              LPWSTR raw = nullptr;
              request->get_Uri(&raw);
              std::wstring uri = raw;
              CoTaskMemFree(raw);
              if (!trusted(uri))
                return S_OK;
              auto route = uri.substr(std::wstring(kOrigin).size());
              const auto cut = route.find_first_of(L"?#");
              if (cut != std::wstring::npos)
                route.resize(cut);
              if (!route.empty() && route.back() == L'/')
                route.pop_back();
              if (route.empty() || route.front() != L'/')
                route.insert(route.begin(), L'/');
              const bool known = route == L"/" || route == L"/library" ||
                                 route == L"/review" || route == L"/stats" ||
                                 route == L"/settings";
              auto relative = fs::path(route == L"/" ? L"" : route.substr(1));
              bool valid = route.find_first_of(L"%\\:") == std::wstring::npos &&
                           !relative.is_absolute();
              for (const auto &part : relative)
                if (part == L".." || part == L".")
                  valid = false;
              const bool document =
                  valid && (known || relative.extension().empty());
              if (document)
                relative = fs::path(L"index.html");
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
                  : extension == L".woff"  ? L"font/woff"
                  : extension == L".ico"   ? L"image/x-icon"
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
              } catch (const std::exception &) {
                return S_OK;
              }
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
                    LR"JS((()=>{let n=0;const timer=setInterval(async()=>{if(++n>100){clearInterval(timer);window.chrome.webview.postMessage({kind:'phase0-smoke',result:{ok:false,error:'UI transport did not initialize'}});return;}if(!window.__LEXICA_INVOKE__)return;clearInterval(timer);try{const invoke=window.__LEXICA_INVOKE__;const info=await invoke('app_info');const suggestions=await invoke('search_suggest',{query:'met',limit:8});const lookup=await invoke('lookup_word',{word:'meticulous'});const caps=await invoke('system_capabilities');const note=caps.commands.find(c=>c.name==='save_word_note');let write='skipped';if(!note.available){try{await invoke('save_word_note',{wordId:lookup.outcome.entry.id,bookmarked:true,note:'smoke'});write='unexpected-success';}catch(e){write=e.code;}}else write='implemented';const ok=info.fts_ok&&info.schema_version===6&&lookup.outcome.type==='hit'&&suggestions.length>0&&write!=='unexpected-success'&&(note.available||write==='NOT_IMPLEMENTED');setTimeout(()=>window.chrome.webview.postMessage({kind:'phase0-smoke',result:{ok,info,suggestions,lookup,write,contract_version:caps.contract_version,title:document.title,ui_rendered:document.body.textContent.includes('meticulous')}}),500);}catch(e){window.chrome.webview.postMessage({kind:'phase0-smoke',result:{ok:false,error:String(e)}});}},100);})())JS";
                webview->ExecuteScript(script, nullptr);
                return S_OK;
              })
              .Get(),
          &token));
    }
    RECT bounds{};
    GetClientRect(window, &bounds);
    controller->put_Bounds(bounds);
    controller->put_IsVisible(FALSE);
    require(webview->add_NavigationCompleted(
        Callback<ICoreWebView2NavigationCompletedEventHandler>(
            [this](ICoreWebView2 *, ICoreWebView2NavigationCompletedEventArgs *args) -> HRESULT {
              BOOL success = FALSE;
              args->get_IsSuccess(&success);
              if (!success) {
                fail("Cannot load the local interface");
                return S_OK;
              }
              interfaceReady = true;
              KillTimer(window, 2);
              SetWindowTextW(window, L"Lexica");
              controller->put_IsVisible(TRUE);
              return S_OK;
            }).Get(), &token));
    require(webview->Navigate(smoke ? L"https://lexica.local/?word=meticulous"
                                    : kOrigin));
  }
  void init() {
    const auto runtime = webViewRuntimeVersion();
    if (runtime.empty())
      throw p::Error("WEBVIEW_ERROR",
                     "WebView2 Runtime is not installed. Install the Evergreen Runtime from https://go.microsoft.com/fwlink/p/?LinkId=2124703");
    if (smoke)
      diagnostics.push_back({{"webview2_runtime", utf8(runtime)}});
    if (!fs::exists(ui / L"index.html") || !fs::exists(ui / L"csp.txt"))
      throw p::Error("RESOURCE_ERROR",
                     "Build the frontend and package its ui folder first");
    startWorker();
  }
  void startWebview() {
    require(CreateCoreWebView2EnvironmentWithOptions(
        nullptr, layout.profile.c_str(), nullptr,
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
  case WM_PAINT:
    if (!host->interfaceReady) {
      host->paintStartup();
      return 0;
    }
    break;
  case WM_CLOSE:
    if (host->startupThread.joinable() &&
        !host->startupDone.load(std::memory_order_acquire)) {
      host->closePending = true;
      host->startupStop.request();
      InvalidateRect(window, nullptr, FALSE);
      return 0;
    }
    break;
  case kStartup:
    try { host->finishStartup(); }
    catch (const std::exception &error) { host->fail(error.what()); }
    return 0;
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
  case WM_DPICHANGED: {
    const auto *suggested = reinterpret_cast<RECT *>(l);
    SetWindowPos(window, nullptr, suggested->left, suggested->top,
                 suggested->right - suggested->left,
                 suggested->bottom - suggested->top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
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
    if (w == 2) {
      InvalidateRect(window, nullptr, FALSE);
      return 0;
    }
    if (w == 1 && host->smoke && !host->finished)
      host->fail("Desktop smoke timeout");
    return 0;
  case WM_DESTROY:
    host->stop();
    PostQuitMessage(host->exitCode);
    return 0;
  default:
    return DefWindowProcW(window, message, w, l);
  }
  return DefWindowProcW(window, message, w, l);
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
    bool haveRoot = false;
    for (int i = 1; i < argc; ++i) {
      std::wstring a = argv[i];
      if (a == L"--data-dir" && i + 1 < argc) {
        host.root = fs::absolute(argv[++i]);
        haveRoot = true;
      } else if (a == L"--ui-dir" && i + 1 < argc)
        host.ui = fs::absolute(argv[++i]);
      else if (a == L"--no-auto-setup")
        host.automaticDictionarySetup = false;
      else if (a == L"--smoke-report" && i + 1 < argc) {
        host.report = fs::absolute(argv[++i]);
        host.smoke = true;
      } else {
        LocalFree(argv);
        throw p::Error("BAD_REQUEST", "Invalid desktop argument");
      }
    }
    LocalFree(argv);
    if (!haveRoot)
      host.root = p::appRoot();
    host.layout = p::resolveLayout(host.root);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    require(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
    WNDCLASSW wc{};
    wc.lpfnWndProc = windowProc;
    wc.hInstance = instance;
    wc.lpszClassName = L"LexicaReproduceParity";
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    const auto iconFile = fs::path(exe).parent_path() / L"lexica.ico";
    if (fs::exists(iconFile))
      wc.hIcon = reinterpret_cast<HICON>(
          LoadImageW(nullptr, iconFile.c_str(), IMAGE_ICON, 0, 0,
                     LR_LOADFROMFILE | LR_DEFAULTSIZE));
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
