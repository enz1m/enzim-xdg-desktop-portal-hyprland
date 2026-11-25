#pragma once

#include <sdbus-c++/sdbus-c++.h>
#include <memory>
#include <string>
#include <unordered_map>
#include "../dbusDefines.hpp"

struct zwlr_virtual_pointer_v1;
struct zwp_virtual_keyboard_v1;
struct xkb_context;
struct xkb_keymap;
struct xkb_state;
struct eis;
struct eis_client;
struct eis_seat;
struct eis_device;
struct eis_event;

// org.freedesktop.impl.portal.RemoteDesktop (version 2) over this backend's
// ScreenCast sessions: the same session can share the screen (chosen in the
// picker) and, only after the person allows it, take keyboard and pointer
// input, through the Notify methods or one EIS connection per session.
class CRemoteDesktopPortal {
  public:
    CRemoteDesktopPortal();
    ~CRemoteDesktopPortal();

    // Called once, after the core's own fds are in the event loop.
    void attachEventLoop();

    enum eDevices : uint32_t {
        DEVICE_KEYBOARD = 1,
        DEVICE_POINTER  = 2,
    };

  private:
    struct SSession {
        uint64_t                 id = 0;
        std::string              appID;
        uint32_t                 requested = 0;
        uint32_t                 granted   = 0;
        bool                     started   = false;
        zwlr_virtual_pointer_v1* pointer   = nullptr;
        zwp_virtual_keyboard_v1* keyboard  = nullptr;
        xkb_state*               state     = nullptr;
        uint32_t                 modifiers[4] = {0, 0, 0, 0};
        // Absolute pointer extent: the shared output's size, 0 when what is
        // shared is not an output.
        uint32_t                 width = 0, height = 0;
        // The session's EIS server; once connected, the Notify methods refuse.
        eis*                     eisContext  = nullptr;
        eis_client*              eisClient   = nullptr;
        eis_seat*                eisSeat     = nullptr;
        eis_device*              eisPointer  = nullptr;
        eis_device*              eisAbsolute = nullptr;
        eis_device*              eisKeyboard = nullptr;
    };

    dbUasv    onCreateSession(sdbus::ObjectPath requestHandle, sdbus::ObjectPath sessionHandle, std::string appID, std::unordered_map<std::string, sdbus::Variant> opts);
    dbUasv    onSelectDevices(sdbus::ObjectPath requestHandle, sdbus::ObjectPath sessionHandle, std::string appID, std::unordered_map<std::string, sdbus::Variant> opts);
    dbUasv    onStart(sdbus::ObjectPath requestHandle, sdbus::ObjectPath sessionHandle, std::string appID, std::string parentWindow,
                      std::unordered_map<std::string, sdbus::Variant> opts);
    sdbus::UnixFd onConnectToEIS(sdbus::ObjectPath sessionHandle, std::string appID, std::unordered_map<std::string, sdbus::Variant> opts);

    // The session, when the method call being handled comes from its creator.
    std::unordered_map<std::string, SSession>::iterator callerSession(sdbus::ObjectPath sessionHandle);
    SSession* active(const std::string& sessionHandle, uint32_t device);
    SSession* notifiable(const std::string& sessionHandle, uint32_t device);
    bool      askControl(const std::string& appID, uint32_t devices, bool offerPersist, bool& persist);
    void      ensureDevices(const std::string& sessionHandle, SSession& session);
    void      forget(const std::string& sessionHandle);
    void      keyboardKey(SSession& session, uint32_t keycode, uint32_t state);
    void      pointerAbsolute(SSession& session, double x, double y);
    void      axis(SSession& session, double dx, double dy, bool finish);
    void      axisDiscrete(SSession& session, uint32_t axis, int32_t steps);
    void      frame(SSession& session);
    void      eisReady();
    void      eisDispatch(const std::string& sessionHandle);
    void      eisEvent(const std::string& sessionHandle, SSession& session, eis_event* event);
    void      eisBind(SSession& session, eis_event* event);
    void      eisClose(SSession& session);
    void      flush();
    uint32_t  now();
    uint32_t  available();

    std::unique_ptr<sdbus::IObject>           m_object;
    std::unordered_map<std::string, SSession> m_sessions;
    uint64_t                                  m_nextID = 1;
    int                                       m_epoll  = -1;
    xkb_context*                              m_xkb    = nullptr;
    xkb_keymap*                               m_keymap = nullptr;
    int                                       m_keymapFd   = -1;
    uint32_t                                  m_keymapSize = 0;
    uint32_t                                  m_consentSerial = 0;

    const sdbus::InterfaceName INTERFACE_NAME = sdbus::InterfaceName{"org.freedesktop.impl.portal.RemoteDesktop"};
    const sdbus::ObjectPath    OBJECT_PATH    = sdbus::ObjectPath{"/org/freedesktop/portal/desktop"};
};
