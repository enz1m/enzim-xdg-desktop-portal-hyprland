#include "enzui-assistant-input-v1-client-protocol.h"
#include "RemoteDesktop.hpp"
#include "Screencopy.hpp"
#include "../core/PortalManager.hpp"
#include "../helpers/Log.hpp"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"
#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include <libeis.h>
#include <xkbcommon/xkbcommon.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <unistd.h>
#include <chrono>
#include <cstring>
#include <ctime>
#include <format>
#include <vector>
#include <algorithm>

// The enzUI portal backend asks the person; this one never decides control
// on its own.
static constexpr const char* CONSENT_SERVICE = "org.freedesktop.impl.portal.desktop.enzui";

static sdbus::Error failure(const std::string& message) {
    return sdbus::Error{sdbus::Error::Name{"org.freedesktop.portal.Error.Failed"}, message};
}

CRemoteDesktopPortal::CRemoteDesktopPortal() {
    m_xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (m_xkb)
        m_keymap = xkb_keymap_new_from_names(m_xkb, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (m_keymap) {
        char* text = xkb_keymap_get_as_string(m_keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
        if (text) {
            m_keymapSize = strlen(text) + 1;
            m_keymapFd   = memfd_create("enzui-remote-keymap", MFD_CLOEXEC | MFD_ALLOW_SEALING);
            if (m_keymapFd >= 0 && write(m_keymapFd, text, m_keymapSize) != (ssize_t)m_keymapSize) {
                close(m_keymapFd);
                m_keymapFd = -1;
            }
            free(text);
        }
    }
    m_epoll = epoll_create1(EPOLL_CLOEXEC);

    m_object = sdbus::createObject(*g_pPortalManager->getConnection(), OBJECT_PATH);
    using Options = std::unordered_map<std::string, sdbus::Variant>;
    m_object
        ->addVTable(
            sdbus::registerMethod("CreateSession")
                .implementedAs([this](sdbus::ObjectPath o1, sdbus::ObjectPath o2, std::string s1, Options m1) { return onCreateSession(o1, o2, s1, m1); }),
            sdbus::registerMethod("SelectDevices")
                .implementedAs([this](sdbus::ObjectPath o1, sdbus::ObjectPath o2, std::string s1, Options m1) { return onSelectDevices(o1, o2, s1, m1); }),
            sdbus::registerMethod("Start").implementedAs(
                [this](sdbus::ObjectPath o1, sdbus::ObjectPath o2, std::string s1, std::string s2, Options m1) { return onStart(o1, o2, s1, s2, m1); }),
            sdbus::registerMethod("ConnectToEIS").implementedAs([this](sdbus::ObjectPath o1, std::string s1, Options m1) { return onConnectToEIS(o1, s1, m1); }),
            sdbus::registerMethod("NotifyPointerMotion").implementedAs([this](sdbus::ObjectPath session, Options, double dx, double dy) {
                if (auto* s = notifiable(session, DEVICE_POINTER)) {
                    zwlr_virtual_pointer_v1_motion(s->pointer, now(), wl_fixed_from_double(dx), wl_fixed_from_double(dy));
                    frame(*s);
                }
            }),
            sdbus::registerMethod("NotifyPointerMotionAbsolute").implementedAs([this](sdbus::ObjectPath session, Options, uint32_t, double x, double y) {
                if (auto* s = notifiable(session, DEVICE_POINTER)) {
                    pointerAbsolute(*s, x, y);
                    frame(*s);
                }
            }),
            sdbus::registerMethod("NotifyPointerButton").implementedAs([this](sdbus::ObjectPath session, Options, int32_t button, uint32_t state) {
                if (auto* s = notifiable(session, DEVICE_POINTER); s && button > 0) {
                    zwlr_virtual_pointer_v1_button(s->pointer, now(), (uint32_t)button, state ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
                    frame(*s);
                }
            }),
            sdbus::registerMethod("NotifyPointerAxis").implementedAs([this](sdbus::ObjectPath session, Options options, double dx, double dy) {
                if (auto* s = notifiable(session, DEVICE_POINTER)) {
                    axis(*s, dx, dy, options.contains("finish") && options["finish"].get<bool>());
                    frame(*s);
                }
            }),
            sdbus::registerMethod("NotifyPointerAxisDiscrete").implementedAs([this](sdbus::ObjectPath session, Options, uint32_t axisIndex, int32_t steps) {
                if (auto* s = notifiable(session, DEVICE_POINTER)) {
                    axisDiscrete(*s, axisIndex, steps);
                    frame(*s);
                }
            }),
            sdbus::registerMethod("NotifyKeyboardKeycode").implementedAs([this](sdbus::ObjectPath session, Options, int32_t keycode, uint32_t state) {
                if (auto* s = notifiable(session, DEVICE_KEYBOARD); s && keycode > 0 && keycode < 0x2ff)
                    keyboardKey(*s, (uint32_t)keycode, state);
            }),
            sdbus::registerMethod("NotifyKeyboardKeysym").implementedAs([this](sdbus::ObjectPath session, Options, int32_t keysym, uint32_t state) {
                auto* s = notifiable(session, DEVICE_KEYBOARD);
                if (!s || !m_keymap)
                    return;
                // The key that types this symbol on the first layout, plain
                // or with Shift.
                for (xkb_keycode_t code = xkb_keymap_min_keycode(m_keymap); code <= xkb_keymap_max_keycode(m_keymap) && code > 8; ++code) {
                    for (xkb_level_index_t level = 0; level < 2; ++level) {
                        const xkb_keysym_t* syms = nullptr;
                        const int           count = xkb_keymap_key_get_syms_by_level(m_keymap, code, 0, level, &syms);
                        if (count != 1 || syms[0] != (xkb_keysym_t)keysym)
                            continue;
                        constexpr uint32_t SHIFT = 42;
                        if (level == 1 && state)
                            keyboardKey(*s, SHIFT, 1);
                        keyboardKey(*s, code - 8, state);
                        if (level == 1 && !state)
                            keyboardKey(*s, SHIFT, 0);
                        return;
                    }
                }
            }),
            sdbus::registerProperty("AvailableDeviceTypes").withGetter([this]() { return available(); }),
            sdbus::registerProperty("version").withGetter([]() { return uint32_t{2}; }))
        .forInterface(INTERFACE_NAME);

    Debug::log(LOG, "[remotedesktop] init successful, devices {}", available());
}

CRemoteDesktopPortal::~CRemoteDesktopPortal() {
    std::vector<std::string> handles;
    for (const auto& [handle, _] : m_sessions)
        handles.push_back(handle);
    for (const auto& handle : handles)
        forget(handle);
    if (m_epoll >= 0)
        close(m_epoll);
    if (m_keymapFd >= 0)
        close(m_keymapFd);
    if (m_keymap)
        xkb_keymap_unref(m_keymap);
    if (m_xkb)
        xkb_context_unref(m_xkb);
}

void CRemoteDesktopPortal::attachEventLoop() {
    // One fd for every session's EIS server: the core's poll list is not
    // changed while its poll thread runs.
    if (m_epoll >= 0)
        g_pPortalManager->addFdToEventLoop(m_epoll, POLLIN, [this]() { eisReady(); });
}

uint32_t CRemoteDesktopPortal::available() {
    const auto& wl = g_pPortalManager->m_sWaylandConnection;
    return (wl.enzuiVirtualKeyboard && wl.enzuiSeat && m_keymapFd >= 0 ? DEVICE_KEYBOARD : 0) | (wl.enzuiVirtualPointer ? DEVICE_POINTER : 0);
}

uint32_t CRemoteDesktopPortal::now() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

void CRemoteDesktopPortal::flush() {
    wl_display_flush(g_pPortalManager->m_sWaylandConnection.display);
}

dbUasv CRemoteDesktopPortal::onCreateSession(sdbus::ObjectPath requestHandle, sdbus::ObjectPath sessionHandle, std::string appID,
                                             std::unordered_map<std::string, sdbus::Variant> opts) {
    auto& screencopy = g_pPortalManager->m_sPortals.screencopy;
    if (!screencopy)
        return {1, {}};
    // The ScreenCast session doubles as this one; SelectSources then picks
    // what is shared exactly as for a plain screen cast.
    auto result = screencopy->onCreateSession(requestHandle, sessionHandle, appID, opts);
    auto* shared = screencopy->getSession(sessionHandle);
    if (std::get<0>(result) != 0 || !shared || !shared->session)
        return result;
    m_sessions[sessionHandle] = SSession{
        .id = m_nextID++, .appID = appID,
        .assistantHandle = sessionHandle.substr(sessionHandle.rfind('/') + 1).starts_with("enzui_assistant_") ? std::string(sessionHandle) : std::string{},
    };
    auto previous             = shared->session->onDestroy;
    const std::string handle  = sessionHandle;
    shared->session->onDestroy = [this, previous, handle]() {
        forget(handle);
        if (previous)
            previous();
    };
    return result;
}

dbUasv CRemoteDesktopPortal::onSelectDevices(sdbus::ObjectPath requestHandle, sdbus::ObjectPath sessionHandle, std::string appID,
                                             std::unordered_map<std::string, sdbus::Variant> opts) {
    const auto it = callerSession(sessionHandle);
    if (it == m_sessions.end() || it->second.started)
        return {1, {}};
    uint32_t types = DEVICE_KEYBOARD | DEVICE_POINTER;
    if (opts.contains("types"))
        types = opts["types"].get<uint32_t>();
    it->second.requested = types & available();
    // The frontend accepts persistence for a remote-desktop session only here, not in SelectSources.
    auto* shared = g_pPortalManager->m_sPortals.screencopy->getSession(sessionHandle);
    if (shared && opts.contains("persist_mode"))
        shared->persistMode = opts["persist_mode"].get<uint32_t>();
    if (shared && opts.contains("restore_data"))
        shared->pendingRestore = g_pPortalManager->m_sPortals.screencopy->redeemRestoreData(opts["restore_data"], shared->appid);
    return {0, {}};
}

dbUasv CRemoteDesktopPortal::onStart(sdbus::ObjectPath requestHandle, sdbus::ObjectPath sessionHandle, std::string appID, std::string parentWindow,
                                     std::unordered_map<std::string, sdbus::Variant> opts) {
    const auto it         = callerSession(sessionHandle);
    auto&      screencopy = g_pPortalManager->m_sPortals.screencopy;
    if (it == m_sessions.end() || it->second.started || !screencopy)
        return {1, {}};
    auto& session = it->second;
    auto* shared  = screencopy->getSession(sessionHandle);
    const bool  sharing  = shared && shared->selection.type != TYPE_INVALID;
    if (!session.assistantHandle.empty() && (sharing || session.requested != (DEVICE_POINTER | DEVICE_KEYBOARD)))
        return {1, {}};
    const auto& restored = shared ? shared->restoredGrant : std::nullopt;
    if (session.requested && restored && (restored->devices & session.requested) == session.requested) {
        session.granted      = session.requested;
        shared->grantDevices = restored->devices;
    } else if (session.requested) {
        // Control is remembered only together with a screen the user chose to keep after a restart.
        const bool offerPersist = shared && shared->selection.persist && shared->persistMode == 2;
        bool       persist      = false;
        session.granted         = askControl(session, sharing, offerPersist, persist) ? session.requested : 0;
        if (shared)
            shared->grantDevices = persist ? session.granted : 0;
    }

    std::unordered_map<std::string, sdbus::Variant> results;
    if (sharing) {
        auto [response, cast] = screencopy->onStart(requestHandle, sessionHandle, appID, parentWindow, opts);
        if (response != 0)
            return {response, {}};
        results = cast;
        if (shared->selection.type == TYPE_OUTPUT) {
            if (const auto* output = g_pPortalManager->getOutputFromName(shared->selection.output)) {
                session.width  = output->width;
                session.height = output->height;
            }
        }
    } else if (!session.granted) {
        return {1, {}};
    } else if (session.granted & DEVICE_POINTER) {
        // Without a screencast, an unbound virtual pointer maps to the desktop layout.
        constexpr uint32_t DESKTOP_EXTENT = 10000;
        session.width                     = DESKTOP_EXTENT;
        session.height                    = DESKTOP_EXTENT;
    }
    session.started    = true;
    results["devices"] = sdbus::Variant{session.granted};
    results["clipboard_enabled"] = sdbus::Variant{false};
    Debug::log(LOG, "[remotedesktop] started {} with devices {}", sessionHandle.c_str(), session.granted);
    return {0, results};
}

sdbus::UnixFd CRemoteDesktopPortal::onConnectToEIS(sdbus::ObjectPath sessionHandle, std::string appID, std::unordered_map<std::string, sdbus::Variant> opts) {
    const auto it = callerSession(sessionHandle);
    if (it == m_sessions.end() || !it->second.started)
        throw failure("No started remote desktop session");
    auto& session = it->second;
    if (session.eisContext || session.assistantScope || session.assistantRevoked)
        throw failure("This session is already connected to EIS or has been revoked");
    if (!session.assistantHandle.empty() && !g_pPortalManager->m_sWaylandConnection.assistantInputGlobal)
        throw failure("The compositor cannot scope Assistant input");
    ensureDevices(sessionHandle, session);
    if (((session.granted & DEVICE_POINTER) && !session.pointer) || ((session.granted & DEVICE_KEYBOARD) && !session.keyboard))
        throw failure("Granted input devices are unavailable");
    if (!session.assistantHandle.empty() && !scopeAssistant(session))
        throw failure("The compositor refused the Assistant input session");
    if (m_epoll < 0)
        throw failure("EIS is unavailable");
    session.eisContext = eis_new(nullptr);
    if (!session.eisContext || eis_setup_backend_fd(session.eisContext) != 0) {
        if (session.eisContext)
            eis_unref(session.eisContext);
        session.eisContext = nullptr;
        throw failure("EIS is unavailable");
    }
    const int fd = eis_backend_fd_add_client(session.eisContext);
    epoll_event watch{.events = EPOLLIN, .data = {.u64 = session.id}};
    if (fd < 0 || epoll_ctl(m_epoll, EPOLL_CTL_ADD, eis_get_fd(session.eisContext), &watch) != 0) {
        if (fd >= 0)
            close(fd);
        eis_unref(session.eisContext);
        session.eisContext = nullptr;
        throw failure("EIS is unavailable");
    }
    Debug::log(LOG, "[remotedesktop] {} connected to EIS with devices {}", sessionHandle.c_str(), session.granted);
    return sdbus::UnixFd{fd, sdbus::adopt_fd};
}

bool CRemoteDesktopPortal::verifyAssistantSession(const std::string& handle) {
    auto&          wl      = g_pPortalManager->m_sWaylandConnection;
    constexpr auto VERSION = ENZUI_ASSISTANT_INPUT_V1_VERIFY_SESSION_SINCE_VERSION;
    if (handle.empty() || wl.assistantInputVersion < VERSION)
        return false;

    auto* verification = static_cast<enzui_assistant_input_v1*>(
        wl_registry_bind(reinterpret_cast<wl_registry*>(wl.registry->resource()), wl.assistantInputGlobal, &enzui_assistant_input_v1_interface, VERSION));
    if (!verification)
        return false;

    static const enzui_assistant_input_v1_listener LISTENER = {
        .attached         = nullptr,
        .revoked          = nullptr,
        .session_verified = [](void* data, enzui_assistant_input_v1*, uint32_t accepted) { *static_cast<bool*>(data) = accepted == 1; },
    };
    bool verified = false;
    enzui_assistant_input_v1_add_listener(verification, &LISTENER, &verified);
    enzui_assistant_input_v1_verify_session(verification, handle.c_str());
    const auto result = wl_display_roundtrip(wl.display);
    enzui_assistant_input_v1_destroy(verification);
    return result >= 0 && verified;
}

bool CRemoteDesktopPortal::askControl(const SSession& session, bool sharing, bool offerPersist, bool& persist) {
    persist = false;
    const bool  assistant = verifyAssistantSession(session.assistantHandle);
    const auto& appID     = session.appID;
    const auto  devices   = session.requested;
    try {
        auto proxy = sdbus::createProxy(*g_pPortalManager->getConnection(), sdbus::ServiceName{CONSENT_SERVICE}, sdbus::ObjectPath{"/org/freedesktop/portal/desktop"});
        const auto handle = sdbus::ObjectPath{std::format("/org/freedesktop/portal/desktop/request/enzui_remote/r{}", ++m_consentSerial)};
        const auto who    = appID.empty() ? std::string{"A remote desktop service"} : appID;
        const auto        what     = devices == (DEVICE_KEYBOARD | DEVICE_POINTER) ? "your keyboard and pointer" : devices == DEVICE_KEYBOARD ? "your keyboard" : "your pointer";
        const auto        title    = assistant ? "Allow the Assistant to control this computer?" : std::format("{} wants to control this computer", who);
        const auto        subtitle = assistant ? "The Assistant in your current conversation will be able to use your keyboard and pointer." :
                                                 std::format("It asks to use {}{}.", what, sharing ? " while the screen is shared" : "");
        const std::string body     = assistant ? "A banner shows while it has control. Click it or press Super+Esc to stop." :
                                                 "Allow it only if you trust whoever is connecting. Remote input is blocked while the screen is locked.";
        std::unordered_map<std::string, sdbus::Variant> options;
        options["grant_label"] = sdbus::Variant{std::string{"Allow control"}};
        options["deny_label"]  = sdbus::Variant{std::string{assistant ? "Don't allow" : sharing ? "View only" : "Deny"}};
        using Choice = sdbus::Struct<std::string, std::string, std::vector<sdbus::Struct<std::string, std::string>>, std::string>;
        if (offerPersist)
            options["choices"] = sdbus::Variant{std::vector<Choice>{Choice{"persist", "Allow after restart", {}, "false"}}};
        uint32_t                                        response = 2;
        std::unordered_map<std::string, sdbus::Variant> results;
        proxy->callMethod("AccessDialog")
            .onInterface("org.freedesktop.impl.portal.Access")
            .withTimeout(std::chrono::minutes(5))
            .withArguments(handle, appID, std::string{}, title, subtitle, body, options)
            .storeResultsTo(response, results);
        if (response != 0)
            return false;
        if (offerPersist && results.contains("choices")) {
            for (const auto& choice : results["choices"].get<std::vector<sdbus::Struct<std::string, std::string>>>())
                persist = persist || (choice.get<0>() == "persist" && choice.get<1>() == "true");
        }
        return true;
    } catch (std::exception& e) {
        Debug::log(ERR, "[remotedesktop] consent unavailable: {}", e.what());
        return false;
    }
}

CRemoteDesktopPortal::SSession* CRemoteDesktopPortal::active(const std::string& sessionHandle, uint32_t device) {
    const auto it = m_sessions.find(sessionHandle);
    if (it == m_sessions.end() || !it->second.started || it->second.assistantRevoked ||
        g_pPortalManager->m_sWaylandConnection.locked || !(it->second.granted & device))
        return nullptr;
    ensureDevices(sessionHandle, it->second);
    if ((device == DEVICE_POINTER && !it->second.pointer) || (device == DEVICE_KEYBOARD && !it->second.keyboard))
        return nullptr;
    return &it->second;
}

std::unordered_map<std::string, CRemoteDesktopPortal::SSession>::iterator CRemoteDesktopPortal::callerSession(sdbus::ObjectPath sessionHandle) {
    auto& screencopy = g_pPortalManager->m_sPortals.screencopy;
    if (!screencopy || !screencopy->callerSession(sessionHandle))
        return m_sessions.end();
    return m_sessions.find(sessionHandle);
}

CRemoteDesktopPortal::SSession* CRemoteDesktopPortal::notifiable(const std::string& sessionHandle, uint32_t device) {
    // A session's input comes either through Notify or through its EIS
    // connection, never both.
    const auto it = callerSession(sdbus::ObjectPath{sessionHandle});
    if (it == m_sessions.end() || it->second.eisContext || !it->second.assistantHandle.empty())
        return nullptr;
    return active(sessionHandle, device);
}

void CRemoteDesktopPortal::ensureDevices(const std::string& sessionHandle, SSession& session) {
    auto& wl = g_pPortalManager->m_sWaylandConnection;
    if ((session.granted & DEVICE_POINTER) && !session.pointer && wl.enzuiVirtualPointer) {
        sdbus::ObjectPath handle{sessionHandle};
        auto*    shared = g_pPortalManager->m_sPortals.screencopy ? g_pPortalManager->m_sPortals.screencopy->getSession(handle) : nullptr;
        SOutput* output = shared && shared->selection.type == TYPE_OUTPUT ? g_pPortalManager->getOutputFromName(shared->selection.output) : nullptr;
        if (output && wl.enzuiVirtualPointerVersion >= 2)
            session.pointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer_with_output(wl.enzuiVirtualPointer, wl.enzuiSeat, (wl_output*)output->output->resource());
        else
            session.pointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(wl.enzuiVirtualPointer, wl.enzuiSeat);
    }
    if ((session.granted & DEVICE_KEYBOARD) && !session.keyboard && wl.enzuiVirtualKeyboard && wl.enzuiSeat && m_keymapFd >= 0) {
        session.keyboard = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(wl.enzuiVirtualKeyboard, wl.enzuiSeat);
        zwp_virtual_keyboard_v1_keymap(session.keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, m_keymapFd, m_keymapSize);
        session.state = xkb_state_new(m_keymap);
    }
    flush();
}

void CRemoteDesktopPortal::keyboardKey(SSession& session, uint32_t keycode, uint32_t state) {
    zwp_virtual_keyboard_v1_key(session.keyboard, now(), keycode, state ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED);
    if (session.state) {
        xkb_state_update_key(session.state, keycode + 8, state ? XKB_KEY_DOWN : XKB_KEY_UP);
        const uint32_t mods[4] = {xkb_state_serialize_mods(session.state, XKB_STATE_MODS_DEPRESSED), xkb_state_serialize_mods(session.state, XKB_STATE_MODS_LATCHED),
                                  xkb_state_serialize_mods(session.state, XKB_STATE_MODS_LOCKED), xkb_state_serialize_layout(session.state, XKB_STATE_LAYOUT_EFFECTIVE)};
        if (memcmp(mods, session.modifiers, sizeof(mods)) != 0) {
            memcpy(session.modifiers, mods, sizeof(mods));
            zwp_virtual_keyboard_v1_modifiers(session.keyboard, mods[0], mods[1], mods[2], mods[3]);
        }
    }
    flush();
}

void CRemoteDesktopPortal::pointerAbsolute(SSession& session, double x, double y) {
    if (!session.width || !session.height)
        return;
    zwlr_virtual_pointer_v1_motion_absolute(session.pointer, now(), (uint32_t)std::clamp(x, 0.0, (double)session.width - 1),
                                            (uint32_t)std::clamp(y, 0.0, (double)session.height - 1), session.width, session.height);
}

void CRemoteDesktopPortal::axis(SSession& session, double dx, double dy, bool finish) {
    const auto time = now();
    if (dx != 0)
        zwlr_virtual_pointer_v1_axis(session.pointer, time, WL_POINTER_AXIS_HORIZONTAL_SCROLL, wl_fixed_from_double(dx));
    if (dy != 0)
        zwlr_virtual_pointer_v1_axis(session.pointer, time, WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_double(dy));
    if (finish) {
        zwlr_virtual_pointer_v1_axis_stop(session.pointer, time, WL_POINTER_AXIS_HORIZONTAL_SCROLL);
        zwlr_virtual_pointer_v1_axis_stop(session.pointer, time, WL_POINTER_AXIS_VERTICAL_SCROLL);
    }
}

void CRemoteDesktopPortal::axisDiscrete(SSession& session, uint32_t axisIndex, int32_t steps) {
    if (axisIndex <= 1 && steps != 0)
        zwlr_virtual_pointer_v1_axis_discrete(session.pointer, now(), axisIndex, wl_fixed_from_int(steps * 15), steps);
}

void CRemoteDesktopPortal::frame(SSession& session) {
    zwlr_virtual_pointer_v1_frame(session.pointer);
    flush();
}

void CRemoteDesktopPortal::eisReady() {
    epoll_event events[8];
    const int   count = epoll_wait(m_epoll, events, 8, 0);
    for (int i = 0; i < count; ++i) {
        for (auto& [handle, session] : m_sessions) {
            if (session.id == events[i].data.u64) {
                eisDispatch(handle);
                break;
            }
        }
    }
}

void CRemoteDesktopPortal::eisDispatch(const std::string& sessionHandle) {
    auto it = m_sessions.find(sessionHandle);
    if (it == m_sessions.end() || !it->second.eisContext)
        return;
    eis_dispatch(it->second.eisContext);
    while (true) {
        it = m_sessions.find(sessionHandle);
        if (it == m_sessions.end() || !it->second.eisContext)
            return;
        eis_event* event = eis_get_event(it->second.eisContext);
        if (!event)
            return;
        eisEvent(sessionHandle, it->second, event);
        eis_event_unref(event);
    }
}

void CRemoteDesktopPortal::eisEvent(const std::string& sessionHandle, SSession& session, eis_event* event) {
    switch (eis_event_get_type(event)) {
        case EIS_EVENT_CLIENT_CONNECT: {
            eis_client* client = eis_event_get_client(event);
            // One sending client per session: the one this fd was handed to.
            if (session.eisClient || !eis_client_is_sender(client)) {
                eis_client_disconnect(client);
                return;
            }
            eis_client_connect(client);
            session.eisClient = eis_client_ref(client);
            session.eisSeat   = eis_client_new_seat(client, "enzUI remote desktop");
            if (session.granted & DEVICE_POINTER) {
                eis_seat_configure_capability(session.eisSeat, EIS_DEVICE_CAP_POINTER);
                if (session.width && session.height)
                    eis_seat_configure_capability(session.eisSeat, EIS_DEVICE_CAP_POINTER_ABSOLUTE);
                eis_seat_configure_capability(session.eisSeat, EIS_DEVICE_CAP_BUTTON);
                eis_seat_configure_capability(session.eisSeat, EIS_DEVICE_CAP_SCROLL);
            }
            if (session.granted & DEVICE_KEYBOARD)
                eis_seat_configure_capability(session.eisSeat, EIS_DEVICE_CAP_KEYBOARD);
            eis_seat_add(session.eisSeat);
            return;
        }
        case EIS_EVENT_CLIENT_DISCONNECT:
            if (eis_event_get_client(event) == session.eisClient) {
                eisClose(session);
                releaseDevices(session);
                if (session.eisClient) {
                    eis_client_unref(session.eisClient);
                    session.eisClient = nullptr;
                }
            }
            return;
        case EIS_EVENT_SEAT_BIND: eisBind(session, event); return;
        case EIS_EVENT_DEVICE_CLOSED: {
            eis_device* device = eis_event_get_device(event);
            for (eis_device** slot : {&session.eisPointer, &session.eisAbsolute, &session.eisKeyboard}) {
                if (*slot == device) {
                    eis_device_remove(device);
                    eis_device_unref(device);
                    *slot = nullptr;
                }
            }
            return;
        }
        default: break;
    }

    // Input from a device this session offered.
    eis_device* device = eis_event_get_device(event);
    if (!device || (device != session.eisPointer && device != session.eisAbsolute && device != session.eisKeyboard))
        return;
    const bool keyboard = device == session.eisKeyboard;
    auto*      s        = active(sessionHandle, keyboard ? DEVICE_KEYBOARD : DEVICE_POINTER);
    if (!s)
        return;
    switch (eis_event_get_type(event)) {
        case EIS_EVENT_POINTER_MOTION:
            zwlr_virtual_pointer_v1_motion(s->pointer, now(), wl_fixed_from_double(eis_event_pointer_get_dx(event)), wl_fixed_from_double(eis_event_pointer_get_dy(event)));
            break;
        case EIS_EVENT_POINTER_MOTION_ABSOLUTE: pointerAbsolute(*s, eis_event_pointer_get_absolute_x(event), eis_event_pointer_get_absolute_y(event)); break;
        case EIS_EVENT_BUTTON_BUTTON:
            zwlr_virtual_pointer_v1_button(s->pointer, now(), eis_event_button_get_button(event),
                                           eis_event_button_get_is_press(event) ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
            break;
        case EIS_EVENT_SCROLL_DELTA: axis(*s, eis_event_scroll_get_dx(event), eis_event_scroll_get_dy(event), false); break;
        case EIS_EVENT_SCROLL_STOP:
        case EIS_EVENT_SCROLL_CANCEL:
            if (eis_event_scroll_get_stop_x(event))
                zwlr_virtual_pointer_v1_axis_stop(s->pointer, now(), WL_POINTER_AXIS_HORIZONTAL_SCROLL);
            if (eis_event_scroll_get_stop_y(event))
                zwlr_virtual_pointer_v1_axis_stop(s->pointer, now(), WL_POINTER_AXIS_VERTICAL_SCROLL);
            break;
        case EIS_EVENT_SCROLL_DISCRETE:
            // 120 per detent.
            axisDiscrete(*s, 1, eis_event_scroll_get_discrete_dy(event) / 120);
            axisDiscrete(*s, 0, eis_event_scroll_get_discrete_dx(event) / 120);
            break;
        case EIS_EVENT_KEYBOARD_KEY: {
            const uint32_t key = eis_event_keyboard_get_key(event);
            if (key > 0 && key < 0x2ff)
                keyboardKey(*s, key, eis_event_keyboard_get_key_is_press(event) ? 1 : 0);
            break;
        }
        case EIS_EVENT_FRAME:
            if (!keyboard)
                frame(*s);
            break;
        default: break;
    }
}

void CRemoteDesktopPortal::eisBind(SSession& session, eis_event* event) {
    auto make = [&](eis_device*& slot, const char* name, std::initializer_list<eis_device_capability> caps) {
        const bool wanted = std::ranges::all_of(caps, [&](auto cap) { return eis_event_seat_has_capability(event, cap); });
        if (!wanted && slot) {
            eis_device_remove(slot);
            eis_device_unref(slot);
            slot = nullptr;
        }
        if (!wanted || slot)
            return wanted;
        slot = eis_seat_new_device(session.eisSeat);
        eis_device_configure_name(slot, name);
        for (auto cap : caps)
            eis_device_configure_capability(slot, cap);
        return true;
    };
    const bool pointer  = (session.granted & DEVICE_POINTER) && !session.eisPointer && make(session.eisPointer, "enzUI remote pointer", {EIS_DEVICE_CAP_POINTER});
    const bool absolute = (session.granted & DEVICE_POINTER) && session.width && session.height && !session.eisAbsolute &&
        make(session.eisAbsolute, "enzUI remote absolute pointer", {EIS_DEVICE_CAP_POINTER_ABSOLUTE});
    const bool keyboard = (session.granted & DEVICE_KEYBOARD) && !session.eisKeyboard && make(session.eisKeyboard, "enzUI remote keyboard", {EIS_DEVICE_CAP_KEYBOARD});
    for (auto [created, device] : {std::pair{pointer, session.eisPointer}, std::pair{absolute, session.eisAbsolute}}) {
        if (!created || !device)
            continue;
        if (eis_event_seat_has_capability(event, EIS_DEVICE_CAP_BUTTON))
            eis_device_configure_capability(device, EIS_DEVICE_CAP_BUTTON);
        if (eis_event_seat_has_capability(event, EIS_DEVICE_CAP_SCROLL))
            eis_device_configure_capability(device, EIS_DEVICE_CAP_SCROLL);
        if (device == session.eisAbsolute) {
            eis_region* region = eis_device_new_region(device);
            eis_region_set_offset(region, 0, 0);
            eis_region_set_size(region, session.width, session.height);
            eis_region_set_physical_scale(region, 1.0);
            eis_region_add(region);
            eis_region_unref(region);
        }
        eis_device_add(device);
        eis_device_resume(device);
    }
    if (keyboard && session.eisKeyboard) {
        if (eis_keymap* keymap = eis_device_new_keymap(session.eisKeyboard, EIS_KEYMAP_TYPE_XKB, m_keymapFd, m_keymapSize)) {
            eis_keymap_add(keymap);
            eis_keymap_unref(keymap);
        }
        eis_device_add(session.eisKeyboard);
        eis_device_resume(session.eisKeyboard);
    }
}

void CRemoteDesktopPortal::eisClose(SSession& session) {
    for (eis_device** slot : {&session.eisPointer, &session.eisAbsolute, &session.eisKeyboard}) {
        if (*slot) {
            eis_device_remove(*slot);
            eis_device_unref(*slot);
            *slot = nullptr;
        }
    }
    if (session.eisSeat) {
        eis_seat_remove(session.eisSeat);
        eis_seat_unref(session.eisSeat);
        session.eisSeat = nullptr;
    }
}

bool CRemoteDesktopPortal::scopeAssistant(SSession& session) {
    auto& wl = g_pPortalManager->m_sWaylandConnection;
    session.assistantScope = static_cast<enzui_assistant_input_v1*>(
        wl_registry_bind(reinterpret_cast<wl_registry*>(wl.registry->resource()), wl.assistantInputGlobal, &enzui_assistant_input_v1_interface, 1));
    if (!session.assistantScope)
        return false;

    static const enzui_assistant_input_v1_listener LISTENER = {
        .attached         = [](void* data, enzui_assistant_input_v1*, uint32_t accepted) { static_cast<SSession*>(data)->assistantAttached = accepted == 1; },
        .revoked          = [](void* data, enzui_assistant_input_v1*) { g_pPortalManager->m_sPortals.remoteDesktop->revokeAssistant(*static_cast<SSession*>(data)); },
        .session_verified = nullptr,
    };
    enzui_assistant_input_v1_add_listener(session.assistantScope, &LISTENER, &session);
    enzui_assistant_input_v1_attach(session.assistantScope, session.assistantHandle.c_str(),
                                   wl_proxy_get_id(reinterpret_cast<wl_proxy*>(session.pointer)), wl_proxy_get_id(reinterpret_cast<wl_proxy*>(session.keyboard)));
    return wl_display_roundtrip(wl.display) >= 0 && session.assistantAttached && !session.assistantRevoked;
}

void CRemoteDesktopPortal::revokeAssistant(SSession& session) {
    session.assistantRevoked = true;
    sdbus::ObjectPath handle{session.assistantHandle};
    auto* shared = g_pPortalManager->m_sPortals.screencopy->getSession(handle);
    if (shared && shared->session)
        closeDBusSession(*shared->session, true);
}

void CRemoteDesktopPortal::suspendInput() {
    for (auto& [_, session] : m_sessions) {
        if (session.assistantHandle.empty())
            releaseDevices(session);
    }
}

void CRemoteDesktopPortal::releaseDevices(SSession& session) {
    if (session.pointer) {
        zwlr_virtual_pointer_v1_destroy(session.pointer);
        session.pointer = nullptr;
    }
    if (session.keyboard) {
        zwp_virtual_keyboard_v1_destroy(session.keyboard);
        session.keyboard = nullptr;
    }
    std::ranges::fill(session.modifiers, 0U);
    if (session.state) {
        xkb_state_unref(session.state);
        session.state = nullptr;
    }
    flush();
}

void CRemoteDesktopPortal::forget(const std::string& sessionHandle) {
    const auto it = m_sessions.find(sessionHandle);
    if (it == m_sessions.end())
        return;
    auto& session = it->second;
    if (session.eisContext) {
        eisClose(session);
        if (session.eisClient) {
            eis_client_disconnect(session.eisClient);
            eis_client_unref(session.eisClient);
        }
        epoll_ctl(m_epoll, EPOLL_CTL_DEL, eis_get_fd(session.eisContext), nullptr);
        eis_unref(session.eisContext);
    }
    releaseDevices(session);
    if (session.assistantScope)
        enzui_assistant_input_v1_destroy(session.assistantScope);
    m_sessions.erase(it);
    flush();
}
