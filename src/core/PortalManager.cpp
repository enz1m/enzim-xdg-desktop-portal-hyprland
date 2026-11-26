#include "PortalManager.hpp"
#include <cstdlib>
#include "../helpers/Log.hpp"
#include "../helpers/MiscFunctions.hpp"
#include "hyprland-lock-notify-v1.hpp"
#include "xdg-output-unstable-v1.hpp"
#include "../shared/Session.hpp"
#include "../portals/RemoteDesktop.hpp"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"
#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include "enzui-assistant-input-v1-client-protocol.h"

#include <pipewire/pipewire.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

#include <thread>

SOutput::SOutput(SP<CCWlOutput> output_) : output(output_) {
    output->setName([this](CCWlOutput* o, const char* name_) {
        if (!name_)
            return;

        name = name_;

        Debug::log(LOG, "Found output name {}", name);
    });
    output->setMode([this](CCWlOutput* r, uint32_t flags, int32_t width_, int32_t height_, int32_t refresh) {
        refreshRate = refresh;
        width       = width_;
        height      = height_;
    });
    output->setGeometry(
        [this](CCWlOutput* r, int32_t x_, int32_t y_, int32_t physical_width, int32_t physical_height, int32_t subpixel, const char* make, const char* model, int32_t transform_) {
            transform = (wl_output_transform)transform_;
            x         = x_;
            y         = y_;
        });
    output->setDescription([this](CCWlOutput* r, const char* description_) { description = description_ ? description_ : ""; });
    output->setScale([this](CCWlOutput* r, uint32_t factor_) { scale = factor_; });
    output->setDone([this](CCWlOutput* r) {
        const auto first = description.find_first_not_of(' ');
        identity         = first == std::string::npos ? std::string{} : description.substr(first, description.find_last_not_of(' ') - first + 1);
        if (g_pPortalManager->m_sPortals.inputCapture != nullptr)
            g_pPortalManager->m_sPortals.inputCapture->zonesChanged();
    });
}

void CPortalManager::setupXDGOutput(SOutput* output) {
    if (!m_sWaylandConnection.xdgOutputManager || !output || output->xdgOutput)
        return;

    output->xdgOutput = makeShared<CCZxdgOutputV1>(m_sWaylandConnection.xdgOutputManager->sendGetXdgOutput(output->output->resource()));

    output->xdgOutput->setLogicalPosition([output](CCZxdgOutputV1* r, int32_t x, int32_t y) {
        output->logicalX             = x;
        output->logicalY             = y;
        output->logicalPositionValid = true;
    });
    output->xdgOutput->setLogicalSize([output](CCZxdgOutputV1* r, int32_t width, int32_t height) {
        output->logicalWidth     = width;
        output->logicalHeight    = height;
        output->logicalSizeValid = true;
    });
    output->xdgOutput->setDone([](CCZxdgOutputV1* r) {
        if (g_pPortalManager->m_sPortals.inputCapture != nullptr)
            g_pPortalManager->m_sPortals.inputCapture->zonesChanged();
    });
}

CPortalManager::CPortalManager() {
    const auto XDG_CONFIG_HOME = getenv("XDG_CONFIG_HOME");
    const auto HOME            = getenv("HOME");

    if (!HOME && !XDG_CONFIG_HOME)
        Debug::log(WARN, "neither $HOME nor $XDG_CONFIG_HOME is present in env");

    std::string path =
        (!XDG_CONFIG_HOME && !HOME) ? "/tmp/xdph.conf" : (XDG_CONFIG_HOME ? std::string{XDG_CONFIG_HOME} + "/hypr/xdph.conf" : std::string{HOME} + "/.config/hypr/xdph.conf");

    m_sConfig.config = std::make_unique<Hyprlang::CConfig>(path.c_str(), Hyprlang::SConfigOptions{.allowMissingConfig = true});

    m_sConfig.config->addConfigValue("general:toplevel_dynamic_bind", Hyprlang::INT{0L});
    m_sConfig.config->addConfigValue("screencopy:max_fps", Hyprlang::INT{120L});
    m_sConfig.config->addConfigValue("screencopy:allow_token_by_default", Hyprlang::INT{0L});
    m_sConfig.config->addConfigValue("screencopy:custom_picker_binary", Hyprlang::STRING{""});
    m_sConfig.config->addConfigValue("screencopy:force_shm", Hyprlang::INT{0L});

    m_sConfig.config->commence();
    m_sConfig.config->parse();
}

void CPortalManager::onGlobal(uint32_t name, const char* interface, uint32_t version) {
    const std::string INTERFACE = interface;

    Debug::log(LOG, " | Got interface: {} (ver {})", INTERFACE, version);

    if (INTERFACE == zwlr_screencopy_manager_v1_interface.name && m_sPipewire.loop) {
        m_sPortals.screencopy = std::make_unique<CScreencopyPortal>(makeShared<CCZwlrScreencopyManagerV1>(
            (wl_proxy*)wl_registry_bind((wl_registry*)m_sWaylandConnection.registry->resource(), name, &zwlr_screencopy_manager_v1_interface, version)));
    }

    if (INTERFACE == enzui_assistant_input_v1_interface.name) {
        m_sWaylandConnection.assistantInputGlobal  = name;
        m_sWaylandConnection.assistantInputVersion = version;
    }

    if (INTERFACE == zwlr_virtual_pointer_manager_v1_interface.name) {
        m_sWaylandConnection.enzuiVirtualPointerVersion = std::min(version, 2u);
        m_sWaylandConnection.enzuiVirtualPointer        = (zwlr_virtual_pointer_manager_v1*)wl_registry_bind((wl_registry*)m_sWaylandConnection.registry->resource(), name,
                                                                                                         &zwlr_virtual_pointer_manager_v1_interface, std::min(version, 2u));
    } else if (INTERFACE == zwp_virtual_keyboard_manager_v1_interface.name) {
        m_sWaylandConnection.enzuiVirtualKeyboard =
            (zwp_virtual_keyboard_manager_v1*)wl_registry_bind((wl_registry*)m_sWaylandConnection.registry->resource(), name, &zwp_virtual_keyboard_manager_v1_interface, 1);
    } else if (INTERFACE == wl_seat_interface.name && !m_sWaylandConnection.enzuiSeat) {
        m_sWaylandConnection.enzuiSeat = (wl_seat*)wl_registry_bind((wl_registry*)m_sWaylandConnection.registry->resource(), name, &wl_seat_interface, 1);
    }

    if (INTERFACE == hyprland_lock_notifier_v1_interface.name) {
        m_sWaylandConnection.lockNotifier = makeShared<CCHyprlandLockNotifierV1>(
            (wl_proxy*)wl_registry_bind((wl_registry*)m_sWaylandConnection.registry->resource(), name, &hyprland_lock_notifier_v1_interface, 1));
        m_sWaylandConnection.lockNotification = makeShared<CCHyprlandLockNotificationV1>(m_sWaylandConnection.lockNotifier->sendGetLockNotification());
        m_sWaylandConnection.lockNotification->setLocked([this](CCHyprlandLockNotificationV1*) {
            m_sWaylandConnection.locked = true;
            if (m_sPortals.remoteDesktop)
                m_sPortals.remoteDesktop->suspendInput();
        });
        m_sWaylandConnection.lockNotification->setUnlocked([this](CCHyprlandLockNotificationV1*) {
            m_sWaylandConnection.locked = false;
            if (!m_sPortals.screencopy && m_sPipewire.loop) {
                m_bCaptureRetried = false;
                requestReconnect();
            }
        });
    }

    if (INTERFACE == hyprland_global_shortcuts_manager_v1_interface.name) {
        m_sPortals.globalShortcuts = std::make_unique<CGlobalShortcutsPortal>(makeShared<CCHyprlandGlobalShortcutsManagerV1>(
            (wl_proxy*)wl_registry_bind((wl_registry*)m_sWaylandConnection.registry->resource(), name, &hyprland_global_shortcuts_manager_v1_interface, version)));
    }
    if (INTERFACE == hyprland_input_capture_manager_v1_interface.name)
        m_sPortals.inputCapture = std::make_unique<CInputCapturePortal>(makeShared<CCHyprlandInputCaptureManagerV1>(
            (wl_proxy*)wl_registry_bind((wl_registry*)m_sWaylandConnection.registry->resource(), name, &hyprland_input_capture_manager_v1_interface, version)));
    else if (INTERFACE == zxdg_output_manager_v1_interface.name) {
        m_sWaylandConnection.xdgOutputManager = makeShared<CCZxdgOutputManagerV1>(
            (wl_proxy*)wl_registry_bind((wl_registry*)m_sWaylandConnection.registry->resource(), name, &zxdg_output_manager_v1_interface, std::min(version, 3u)));

        for (auto& output : m_vOutputs)
            setupXDGOutput(output.get());
    } else if (INTERFACE == hyprland_toplevel_export_manager_v1_interface.name) {
        m_sWaylandConnection.hyprlandToplevelMgr = makeShared<CCHyprlandToplevelExportManagerV1>(
            (wl_proxy*)wl_registry_bind((wl_registry*)m_sWaylandConnection.registry->resource(), name, &hyprland_toplevel_export_manager_v1_interface, version));
    }

    else if (INTERFACE == wl_output_interface.name) {
        const auto POUTPUT = m_vOutputs
                                 .emplace_back(std::make_unique<SOutput>(makeShared<CCWlOutput>(
                                     (wl_proxy*)wl_registry_bind((wl_registry*)m_sWaylandConnection.registry->resource(), name, &wl_output_interface, version))))
                                 .get();
        POUTPUT->id        = name;
        setupXDGOutput(POUTPUT);
    }

    else if (INTERFACE == zwp_linux_dmabuf_v1_interface.name) {
        if (version < 4) {
            Debug::log(ERR, "cannot use linux_dmabuf with ver < 4");
            return;
        }

        m_sWaylandConnection.linuxDmabuf =
            makeShared<CCZwpLinuxDmabufV1>((wl_proxy*)wl_registry_bind((wl_registry*)m_sWaylandConnection.registry->resource(), name, &zwp_linux_dmabuf_v1_interface, version));
        m_sWaylandConnection.linuxDmabufFeedback = makeShared<CCZwpLinuxDmabufFeedbackV1>(m_sWaylandConnection.linuxDmabuf->sendGetDefaultFeedback());

        m_sWaylandConnection.linuxDmabufFeedback->setMainDevice([this](CCZwpLinuxDmabufFeedbackV1* r, wl_array* device_arr) {
            Debug::log(LOG, "[core] dmabufFeedbackMainDevice");

            if (m_sWaylandConnection.dma.done)
                return;

            RASSERT(!m_sWaylandConnection.gbm, "double dmabuf feedback");

            dev_t device;
            assert(device_arr->size == sizeof(device));
            memcpy(&device, device_arr->data, sizeof(device));

            drmDevice* drmDev;
            if (drmGetDeviceFromDevId(device, /* flags */ 0, &drmDev) != 0) {
                Debug::log(WARN, "[dmabuf] unable to open main device?");
                exit(1);
            }

            m_sWaylandConnection.gbmDevice = createGBMDevice(drmDev);
        });
        m_sWaylandConnection.linuxDmabufFeedback->setFormatTable([this](CCZwpLinuxDmabufFeedbackV1* r, int fd, uint32_t size) {
            Debug::log(TRACE, "[core] dmabufFeedbackFormatTable");

            if (m_sWaylandConnection.dma.done)
                return;

            m_vDMABUFMods.clear();

            m_sWaylandConnection.dma.formatTable = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);

            if (m_sWaylandConnection.dma.formatTable == MAP_FAILED) {
                Debug::log(ERR, "[core] format table failed to mmap");
                m_sWaylandConnection.dma.formatTable     = nullptr;
                m_sWaylandConnection.dma.formatTableSize = 0;
                return;
            }

            m_sWaylandConnection.dma.formatTableSize = size;
        });
        m_sWaylandConnection.linuxDmabufFeedback->setDone([this](CCZwpLinuxDmabufFeedbackV1* r) {
            Debug::log(TRACE, "[core] dmabufFeedbackDone");

            if (m_sWaylandConnection.dma.done)
                return;

            if (m_sWaylandConnection.dma.formatTable)
                munmap(m_sWaylandConnection.dma.formatTable, m_sWaylandConnection.dma.formatTableSize);

            m_sWaylandConnection.dma.formatTable     = nullptr;
            m_sWaylandConnection.dma.formatTableSize = 0;
            m_sWaylandConnection.dma.done            = true;
        });
        m_sWaylandConnection.linuxDmabufFeedback->setTrancheTargetDevice([this](CCZwpLinuxDmabufFeedbackV1* r, wl_array* device_arr) {
            Debug::log(TRACE, "[core] dmabufFeedbackTrancheTargetDevice");

            if (m_sWaylandConnection.dma.done)
                return;

            dev_t device;
            assert(device_arr->size == sizeof(device));
            memcpy(&device, device_arr->data, sizeof(device));

            drmDevice* drmDev;
            if (drmGetDeviceFromDevId(device, /* flags */ 0, &drmDev) != 0)
                return;

            if (m_sWaylandConnection.gbmDevice) {
                drmDevice* drmDevRenderer = NULL;
                drmGetDevice2(gbm_device_get_fd(m_sWaylandConnection.gbmDevice), /* flags */ 0, &drmDevRenderer);
                m_sWaylandConnection.dma.deviceUsed = drmDevicesEqual(drmDevRenderer, drmDev);
            } else {
                m_sWaylandConnection.gbmDevice      = createGBMDevice(drmDev);
                m_sWaylandConnection.dma.deviceUsed = m_sWaylandConnection.gbm;
            }
        });
        m_sWaylandConnection.linuxDmabufFeedback->setTrancheFormats([this](CCZwpLinuxDmabufFeedbackV1* r, wl_array* indices) {
            Debug::log(TRACE, "[core] dmabufFeedbackTrancheFormats");

            if (m_sWaylandConnection.dma.done)
                return;

            if (!m_sWaylandConnection.dma.deviceUsed || !m_sWaylandConnection.dma.formatTable)
                return;

            struct fm_entry {
                uint32_t format;
                uint32_t padding;
                uint64_t modifier;
            };
            // An entry in the table has to be 16 bytes long
            assert(sizeof(struct fm_entry) == 16);

            uint32_t  n_modifiers = m_sWaylandConnection.dma.formatTableSize / sizeof(struct fm_entry);
            fm_entry* fm_entry    = (struct fm_entry*)m_sWaylandConnection.dma.formatTable;
            uint16_t* idx;

            for (idx = (uint16_t*)indices->data; (const char*)idx < (const char*)indices->data + indices->size; idx++) {
                if (*idx >= n_modifiers)
                    continue;

                m_vDMABUFMods.push_back({(fm_entry + *idx)->format, (fm_entry + *idx)->modifier});
            }
        });
        m_sWaylandConnection.linuxDmabufFeedback->setTrancheDone([this](CCZwpLinuxDmabufFeedbackV1* r) {
            Debug::log(TRACE, "[core] dmabufFeedbackTrancheDone");

            if (m_sWaylandConnection.dma.done)
                return;

            m_sWaylandConnection.dma.deviceUsed = false;
        });

    }

    else if (INTERFACE == wl_shm_interface.name)
        m_sWaylandConnection.shm = makeShared<CCWlShm>((wl_proxy*)wl_registry_bind((wl_registry*)m_sWaylandConnection.registry->resource(), name, &wl_shm_interface, version));

    else if (INTERFACE == zwlr_foreign_toplevel_manager_v1_interface.name) {
        m_sHelpers.toplevel = std::make_unique<CToplevelManager>(name, version);

        // remove when another fix is found for https://github.com/hyprwm/xdg-desktop-portal-hyprland/issues/147
        if (!std::any_cast<Hyprlang::INT>(m_sConfig.config->getConfigValue("general:toplevel_dynamic_bind")))
            m_sHelpers.toplevel->activate();
    }

    else if (INTERFACE == hyprland_toplevel_mapping_manager_v1_interface.name) {
        m_sHelpers.toplevelMapping = std::make_unique<CToplevelMappingManager>(makeShared<CCHyprlandToplevelMappingManagerV1>(
            (wl_proxy*)wl_registry_bind((wl_registry*)m_sWaylandConnection.registry->resource(), name, &hyprland_toplevel_mapping_manager_v1_interface, version)));
    }
}

void CPortalManager::onGlobalRemoved(uint32_t name) {
    std::erase_if(m_vOutputs, [&](const auto& other) { return other->id == name; });
}

int CPortalManager::init() {
    m_iPID = getpid();

    try {
        // enzUI: a build that adds RemoteDesktop runs beside the system's
        // own xdph under the name its portal file gives.
        const char* busName = getenv("XDPH_BUS_NAME");
        m_pConnection       = sdbus::createSessionBusConnection(sdbus::ServiceName{busName && *busName ? busName : "org.freedesktop.impl.portal.desktop.hyprland"});
    } catch (std::exception& e) {
        Debug::log(CRIT, "Couldn't create the dbus connection ({})", e.what());
        return EXIT_FAILURE;
    }

    if (!m_pConnection) {
        Debug::log(CRIT, "Couldn't connect to dbus");
        return EXIT_FAILURE;
    }

    if (const auto ENV = getenv("XDG_CURRENT_DESKTOP"); ENV) {
        Debug::log(LOG, "XDG_CURRENT_DESKTOP set to {}", ENV);

        if (std::string(ENV) != "Hyprland")
            Debug::log(WARN, "Not running on hyprland, some features might be unavailable");
    } else {
        Debug::log(WARN, "XDG_CURRENT_DESKTOP unset, running on an unknown desktop");
    }

    m_iWakeFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (m_iWakeFd < 0) {
        Debug::log(CRIT, "Couldn't create the event loop wakeup ({})", strerror(errno));
        return EXIT_FAILURE;
    }

    pw_init(nullptr, nullptr);
    m_sPipewire.loop = pw_loop_new(nullptr);

    if (!m_sPipewire.loop)
        Debug::log(ERR, "Pipewire: refused to create a loop. Screensharing will not work.");

    int                                                status = EXIT_SUCCESS;
    std::vector<std::chrono::steady_clock::time_point> reconnects;
    for (bool first = true;; first = false) {
        const auto CONNECTED = connectWayland();
        if (CONNECTED != eConnect::CONNECTED) {
            // A compositor that goes away after startup ends the session normally.
            status = CONNECTED == eConnect::NO_COMPOSITOR && !first ? EXIT_SUCCESS : EXIT_FAILURE;
            break;
        }

        const auto END = runEventLoop();
        disconnectWayland();
        if (END == eLoopEnd::TERMINATED)
            break;

        const auto NOW = std::chrono::steady_clock::now();
        std::erase_if(reconnects, [NOW](const auto& at) { return NOW - at > std::chrono::seconds(10); });
        reconnects.push_back(NOW);
        if (reconnects.size() > 10) {
            Debug::log(CRIT, "[core] Wayland connection keeps failing, exiting");
            status = EXIT_FAILURE;
            break;
        }
        Debug::log(LOG, "[core] Reconnecting to the compositor");
    }

    m_pConnection.reset();
    if (m_sPipewire.loop)
        pw_loop_destroy(m_sPipewire.loop);
    m_sPipewire.loop = nullptr;
    close(m_iWakeFd);
    m_iWakeFd = -1;

    return status;
}

CPortalManager::eConnect CPortalManager::connectWayland() {
    m_sWaylandConnection.display = wl_display_connect(nullptr);

    if (!m_sWaylandConnection.display) {
        Debug::log(CRIT, "Couldn't connect to a wayland compositor");
        return eConnect::NO_COMPOSITOR;
    }

    m_sWaylandConnection.registry = makeShared<CCWlRegistry>((wl_proxy*)wl_display_get_registry(m_sWaylandConnection.display));
    m_sWaylandConnection.registry->setGlobal([this](CCWlRegistry* r, uint32_t name, const char* iface, uint32_t ver) { onGlobal(name, iface, ver); });
    m_sWaylandConnection.registry->setGlobalRemove([this](CCWlRegistry* r, uint32_t name) { onGlobalRemoved(name); });

    Debug::log(LOG, "Gathering exported interfaces");

    // A second roundtrip lets per-output protocol objects created from registry
    // globals, such as xdg_output, receive their initial state.
    if (wl_display_roundtrip(m_sWaylandConnection.display) < 0 || wl_display_roundtrip(m_sWaylandConnection.display) < 0) {
        Debug::log(CRIT, "Lost the wayland compositor while gathering interfaces");
        disconnectWayland();
        return eConnect::NO_COMPOSITOR;
    }

    // enzUI: the compositor withholds capture while the session is locked and
    // offers it only to connections made after the unlock.
    if (m_sPortals.screencopy) {
        m_bCaptureRetried = false;
        if (m_sWaylandConnection.hyprlandToplevelMgr)
            m_sPortals.screencopy->appendToplevelExport(m_sWaylandConnection.hyprlandToplevelMgr);
    } else if (m_sWaylandConnection.locked)
        Debug::log(LOG, "Screencopy waits for the session to unlock");
    else if (m_sWaylandConnection.lockNotification && m_sPipewire.loop && !m_bCaptureRetried) {
        // A lock is announced only once its screen is shown, and a lock that
        // ends before then is never announced.
        Debug::log(LOG, "Screencopy withheld, retrying shortly");
        m_bCaptureRetried = true;
        addTimer({6000, [this]() {
                      if (!m_sPortals.screencopy && !m_sWaylandConnection.locked)
                          requestReconnect();
                  }});
    } else if (getenv("XDPH_BUS_NAME")) {
        // enzUI: this instance exists only to share the screen.
        Debug::log(CRIT, "Screencopy unavailable (capture not admitted), exiting");
        disconnectWayland();
        return eConnect::NOT_ADMITTED;
    } else
        Debug::log(WARN, "Screencopy not started: compositor doesn't support zwlr_screencopy_v1 or pw refused a loop");

    if (const char* busName = getenv("XDPH_BUS_NAME"); m_sPortals.screencopy && busName && *busName)
        m_sPortals.remoteDesktop = std::make_unique<CRemoteDesktopPortal>(); // enzUI: only under its own name

    if (!inShellPath("grim"))
        Debug::log(WARN, "grim not found. Screenshots will not work.");
    else {
        m_sPortals.screenshot = std::make_unique<CScreenshotPortal>();

        if (!inShellPath("slurp"))
            Debug::log(WARN, "slurp not found. You won't be able to select a region when screenshotting.");

        if (!inShellPath("slurp") && !inShellPath("hyprpicker"))
            Debug::log(WARN, "Neither slurp nor hyprpicker found. You won't be able to pick colors.");
        else if (!inShellPath("hyprpicker"))
            Debug::log(INFO, "hyprpicker not found. We suggest to use hyprpicker for color picking to be less meh.");
    }

    wl_display_roundtrip(m_sWaylandConnection.display);
    return eConnect::CONNECTED;
}

void CPortalManager::disconnectWayland() {
    // Frontend sessions end with the portals that serve them.
    closeAllDBusSessions();

    m_sPortals.globalShortcuts.reset();
    m_sPortals.remoteDesktop.reset();
    if (m_sPortals.screencopy)
        m_sPortals.screencopy->shutdown();
    m_sPortals.screencopy.reset();
    m_sPortals.screenshot.reset();
    m_sHelpers.toplevel.reset();
    m_sHelpers.toplevelMapping.reset();
    m_sPortals.inputCapture.reset();
    m_sTimersThread.timers.clear();
    m_sEventLoopInternals.pollCallbacks.clear();
    m_sEventLoopInternals.pollFds.clear();

    // Protocol destructors marshal destroy requests and need a live display object.
    m_vOutputs.clear();
    m_sWaylandConnection.lockNotification.reset();
    m_sWaylandConnection.lockNotifier.reset();
    m_sWaylandConnection.locked = false;
    m_sWaylandConnection.linuxDmabufFeedback.reset();
    m_sWaylandConnection.linuxDmabuf.reset();
    m_sWaylandConnection.hyprlandToplevelMgr.reset();
    m_sWaylandConnection.xdgOutputManager.reset();
    m_sWaylandConnection.shm.reset();
    m_sWaylandConnection.registry.reset();
    if (m_sWaylandConnection.enzuiVirtualPointer)
        zwlr_virtual_pointer_manager_v1_destroy(m_sWaylandConnection.enzuiVirtualPointer);
    if (m_sWaylandConnection.enzuiVirtualKeyboard)
        zwp_virtual_keyboard_manager_v1_destroy(m_sWaylandConnection.enzuiVirtualKeyboard);
    if (m_sWaylandConnection.enzuiSeat)
        wl_seat_destroy(m_sWaylandConnection.enzuiSeat);
    m_sWaylandConnection.enzuiVirtualPointer        = nullptr;
    m_sWaylandConnection.enzuiVirtualPointerVersion = 0;
    m_sWaylandConnection.assistantInputGlobal       = 0;
    m_sWaylandConnection.assistantInputVersion      = 0;
    m_sWaylandConnection.enzuiVirtualKeyboard       = nullptr;
    m_sWaylandConnection.enzuiSeat                  = nullptr;

    if (m_sWaylandConnection.gbmDevice) {
        const int FD = gbm_device_get_fd(m_sWaylandConnection.gbmDevice);
        gbm_device_destroy(m_sWaylandConnection.gbmDevice);
        close(FD);
    }
    m_sWaylandConnection.gbmDevice = nullptr;
    if (m_sWaylandConnection.dma.formatTable)
        munmap(m_sWaylandConnection.dma.formatTable, m_sWaylandConnection.dma.formatTableSize);
    m_sWaylandConnection.dma = {};
    m_vDMABUFMods.clear();

    if (m_sWaylandConnection.display)
        wl_display_disconnect(m_sWaylandConnection.display);
    m_sWaylandConnection.display = nullptr;
}

bool CPortalManager::stopping() const {
    return m_bTerminate || m_bReconnect;
}

void CPortalManager::wakePollThread() {
    if (m_iWakeFd >= 0)
        (void)eventfd_write(m_iWakeFd, 1);
}

CPortalManager::eLoopEnd CPortalManager::runEventLoop() {
    eventfd_t drained = 0;
    (void)eventfd_read(m_iWakeFd, &drained);

    addFdToEventLoop(m_pConnection->getEventLoopPollData().fd, POLLIN, nullptr);
    addFdToEventLoop(wl_display_get_fd(m_sWaylandConnection.display), POLLIN, nullptr);
    addFdToEventLoop(pw_loop_get_fd(m_sPipewire.loop), POLLIN, nullptr);
    addFdToEventLoop(m_iWakeFd, POLLIN, nullptr);
    if (m_sPortals.remoteDesktop)
        m_sPortals.remoteDesktop->attachEventLoop();

    std::thread pollThr([this]() {
        while (1) {

            int ret = poll(m_sEventLoopInternals.pollFds.data(), m_sEventLoopInternals.pollFds.size(), 5000 /* 5 seconds, reasonable. It's because we might need to terminate */);
            if (ret < 0 && errno != EINTR) {
                Debug::log(CRIT, "[core] Polling fds failed with {}", strerror(errno));
                g_pPortalManager->terminate();
            }

            // enzUI: the compositor revokes this portal's Wayland connection
            // when the screen locks; D-Bus or PipeWire loss ends the portal.
            for (size_t i = 0; i < 3; ++i) {
                if (m_sEventLoopInternals.pollFds[i].revents & (POLLHUP | POLLERR)) {
                    Debug::log(CRIT, "[core] Disconnected from pollfd id {}", i);
                    if (i == 1)
                        g_pPortalManager->requestReconnect();
                    else
                        g_pPortalManager->terminate();
                }
            }

            if (stopping())
                break;

            if (ret > 0) {
                Debug::log(TRACE, "[core] got poll event");
                std::lock_guard<std::mutex> lg(m_sEventLoopInternals.loopRequestMutex);
                m_sEventLoopInternals.shouldProcess = true;
                m_sEventLoopInternals.loopSignal.notify_all();
            }
        }
    });

    m_sTimersThread.thread = std::make_unique<std::thread>([this] {
        while (1) {
            std::unique_lock lk(m_sTimersThread.loopMutex);

            // find nearest timer ms
            m_mEventLock.lock();
            float nearest = 60000; /* reasonable timeout */
            for (auto& t : m_sTimersThread.timers) {
                float until = t->duration() - t->passedMs();
                if (until < nearest)
                    nearest = until;
            }
            m_mEventLock.unlock();

            m_sTimersThread.loopSignal.wait_for(lk, std::chrono::milliseconds((int)nearest), [this] { return m_sTimersThread.shouldProcess.load() || stopping(); });
            m_sTimersThread.shouldProcess = false;

            if (stopping())
                break;

            // awakened. Check if any timers passed
            m_mEventLock.lock();
            bool notify = false;
            for (auto& t : m_sTimersThread.timers) {
                if (t->passed()) {
                    Debug::log(TRACE, "[core] got timer event");
                    notify = true;
                    break;
                }
            }
            m_mEventLock.unlock();

            if (notify) {
                std::lock_guard<std::mutex> lg(m_sEventLoopInternals.loopRequestMutex);
                m_sEventLoopInternals.shouldProcess = true;
                m_sEventLoopInternals.loopSignal.notify_all();
            }
        }
    });

    while (1) { // dbus events
        // wait for being awakened
        std::unique_lock lk(m_sEventLoopInternals.loopMutex);
        if (m_sEventLoopInternals.shouldProcess == false) // avoid a lock if a thread managed to request something already since we .unlock()ed
            m_sEventLoopInternals.loopSignal.wait_for(lk, std::chrono::seconds(5), [this] { return m_sEventLoopInternals.shouldProcess == true || stopping(); }); // wait for events

        std::lock_guard<std::mutex> lg(m_sEventLoopInternals.loopRequestMutex);

        if (stopping())
            break;

        m_sEventLoopInternals.shouldProcess = false;

        m_mEventLock.lock();

        if (m_sEventLoopInternals.pollFds[0].revents & POLLIN /* dbus */) {
            while (m_pConnection->processPendingEvent()) {
                ;
            }
        }

        if (m_sEventLoopInternals.pollFds[1].revents & POLLIN /* wl */) {
            wl_display_flush(m_sWaylandConnection.display);
            if (wl_display_prepare_read(m_sWaylandConnection.display) == 0) {
                wl_display_read_events(m_sWaylandConnection.display);
                wl_display_dispatch_pending(m_sWaylandConnection.display);
            } else {
                wl_display_dispatch(m_sWaylandConnection.display);
            }
        }

        if (m_sEventLoopInternals.pollFds[2].revents & POLLIN /* pw */) {
            while (pw_loop_iterate(m_sPipewire.loop, 0) != 0) {
                ;
            }
        }

        for (pollfd p : m_sEventLoopInternals.pollFds) {
            if (p.revents & POLLIN && m_sEventLoopInternals.pollCallbacks.contains(p.fd)) {
                m_sEventLoopInternals.pollCallbacks[p.fd]();
            }
        }

        std::vector<CTimer*> toRemove;
        for (auto& t : m_sTimersThread.timers) {
            if (t->passed()) {
                t->m_fnCallback();
                toRemove.emplace_back(t.get());
                Debug::log(TRACE, "[core] calling timer {}", (void*)t.get());
            }
        }

        int ret = 0;
        do {
            ret = wl_display_dispatch_pending(m_sWaylandConnection.display);
            wl_display_flush(m_sWaylandConnection.display);
        } while (ret > 0);

        if (const int error = wl_display_get_error(m_sWaylandConnection.display); error != 0 && !m_bTerminate) {
            // enzUI: streams and devices on a lost connection are dead, so the portals are rebuilt.
            Debug::log(CRIT, "[core] Wayland connection lost ({})", strerror(error));
            requestReconnect();
        }

        if (!toRemove.empty())
            std::erase_if(m_sTimersThread.timers,
                          [&](const auto& t) { return std::find_if(toRemove.begin(), toRemove.end(), [&](const auto& other) { return other == t.get(); }) != toRemove.end(); });

        m_mEventLock.unlock();
    }

    // Stop callbacks before releasing portals, their timers or connection fds.
    pollThr.join();
    if (m_sTimersThread.thread && m_sTimersThread.thread->joinable())
        m_sTimersThread.thread->join();
    m_sTimersThread.thread.reset();

    if (m_bTerminate) {
        Debug::log(ERR, "[core] Terminated");
        return eLoopEnd::TERMINATED;
    }

    m_bReconnect = false;
    return eLoopEnd::RECONNECT;
}

sdbus::IConnection* CPortalManager::getConnection() {
    return m_pConnection.get();
}

std::string CPortalManager::caller() {
    const char* sender = m_pConnection->getCurrentlyProcessedMessage().getSender();
    return sender ? sender : "";
}

std::string CPortalManager::requireFrontend() {
    const auto  sender = caller();
    std::string owner;
    try {
        auto bus = sdbus::createProxy(*m_pConnection, sdbus::ServiceName{"org.freedesktop.DBus"}, sdbus::ObjectPath{"/org/freedesktop/DBus"});
        bus->callMethod("GetNameOwner").onInterface("org.freedesktop.DBus").withArguments(std::string{"org.freedesktop.portal.Desktop"}).storeResultsTo(owner);
    } catch (std::exception& e) { Debug::log(ERR, "[core] portal frontend owner unknown: {}", e.what()); }

    if (sender.empty() || sender != owner) {
        Debug::log(WARN, "[core] refused a call from {}: not the portal frontend", sender);
        throw sdbus::Error{sdbus::Error::Name{"org.freedesktop.DBus.Error.AccessDenied"}, "Only the portal frontend may call this backend"};
    }
    return sender;
}

SOutput* CPortalManager::getOutputFromName(const std::string& name) {
    for (auto& o : m_vOutputs) {
        if (o->name == name)
            return o.get();
    }
    return nullptr;
}

std::vector<std::unique_ptr<SOutput>> const& CPortalManager::getAllOutputs() {
    return m_vOutputs;
}

static char* gbm_find_render_node(drmDevice* device) {
    drmDevice* devices[64];
    char*      render_node = NULL;

    int        n = drmGetDevices2(0, devices, sizeof(devices) / sizeof(devices[0]));
    for (int i = 0; i < n; ++i) {
        drmDevice* dev = devices[i];
        if (device && !drmDevicesEqual(device, dev)) {
            continue;
        }
        if (!(dev->available_nodes & (1 << DRM_NODE_RENDER)))
            continue;

        render_node = strdup(dev->nodes[DRM_NODE_RENDER]);
        break;
    }

    drmFreeDevices(devices, n);
    return render_node;
}

gbm_device* CPortalManager::createGBMDevice(drmDevice* dev) {
    char* renderNode = gbm_find_render_node(dev);

    if (!renderNode) {
        Debug::log(ERR, "[core] Couldn't find a render node");
        return nullptr;
    }

    Debug::log(TRACE, "[core] createGBMDevice: render node {}", renderNode);

    int fd = open(renderNode, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        Debug::log(ERR, "[core] couldn't open render node");
        free(renderNode);
        return NULL;
    }

    free(renderNode);
    return gbm_create_device(fd);
}

void CPortalManager::addTimer(const CTimer& timer) {
    Debug::log(TRACE, "[core] adding timer for {}ms", timer.duration());
    m_sTimersThread.timers.emplace_back(std::make_unique<CTimer>(timer));
    m_sTimersThread.shouldProcess = true;
    m_sTimersThread.loopSignal.notify_all();
}

void CPortalManager::addFdToEventLoop(int fd, short events, std::function<void()> callback) {
    m_sEventLoopInternals.pollFds.emplace_back(pollfd{.fd = fd, .events = POLLIN});

    if (callback == nullptr)
        return;

    m_sEventLoopInternals.pollCallbacks[fd] = callback;
}

void CPortalManager::removeFdFromEventLoop(int fd) {
    std::erase_if(m_sEventLoopInternals.pollFds, [fd](const pollfd& p) { return p.fd == fd; });
    m_sEventLoopInternals.pollCallbacks.erase(fd);
}

void CPortalManager::terminate() {
    if (m_bTerminate.exchange(true))
        return;

    m_sEventLoopInternals.shouldProcess = true;
    m_sEventLoopInternals.loopSignal.notify_all();
    m_sTimersThread.shouldProcess = true;
    m_sTimersThread.loopSignal.notify_all();
    wakePollThread();
}

void CPortalManager::requestReconnect() {
    if (m_bTerminate || m_bReconnect.exchange(true))
        return;

    m_sEventLoopInternals.shouldProcess = true;
    m_sEventLoopInternals.loopSignal.notify_all();
    m_sTimersThread.shouldProcess = true;
    m_sTimersThread.loopSignal.notify_all();
    wakePollThread();
}
