#include "Session.hpp"
#include "../core/PortalManager.hpp"
#include "../helpers/Log.hpp"

static void requireOwner(const std::string& owner) {
    if (g_pPortalManager->caller() != owner)
        throw sdbus::Error{sdbus::Error::Name{"org.freedesktop.DBus.Error.AccessDenied"}, "Not the creator of this object"};
}

static int onCloseRequest(SDBusRequest* req) {
    Debug::log(TRACE, "[internal] Close Request {}", (void*)req);

    if (!req)
        return 0;

    requireOwner(req->owner);

    req->onDestroy();
    req->object.release();

    return 0;
}

static int onCloseSession(SDBusSession* sess) {
    Debug::log(TRACE, "[internal] Close Session {}", (void*)sess);

    if (!sess)
        return 0;

    requireOwner(sess->owner);

    auto destroy = std::move(sess->onDestroy);
    g_pPortalManager->addTimer({0, [sess, destroy]() {
                                    sess->object.reset();
                                    if (destroy)
                                        destroy();
                                }});

    return 0;
}

std::unique_ptr<SDBusSession> createDBusSession(sdbus::ObjectPath handle) {
    Debug::log(TRACE, "[internal] Create Session {}", handle.c_str());

    std::unique_ptr<SDBusSession> pSession = std::make_unique<SDBusSession>();
    const auto                    PSESSION = pSession.get();
    pSession->owner                        = g_pPortalManager->caller();

    pSession->object = sdbus::createObject(*g_pPortalManager->getConnection(), handle);

    pSession->object->addVTable(sdbus::registerMethod("Close").implementedAs([PSESSION]() { onCloseSession(PSESSION); })).forInterface("org.freedesktop.impl.portal.Session");

    return pSession;
}

std::unique_ptr<SDBusRequest> createDBusRequest(sdbus::ObjectPath handle) {
    Debug::log(TRACE, "[internal] Create Request {}", handle.c_str());

    std::unique_ptr<SDBusRequest> pRequest = std::make_unique<SDBusRequest>();
    const auto                    PREQUEST = pRequest.get();
    pRequest->owner                        = g_pPortalManager->caller();

    pRequest->object = sdbus::createObject(*g_pPortalManager->getConnection(), handle);

    pRequest->object->addVTable(sdbus::registerMethod("Close").implementedAs([PREQUEST]() { onCloseRequest(PREQUEST); })).forInterface("org.freedesktop.impl.portal.Request");

    return pRequest;
}