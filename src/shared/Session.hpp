#pragma once

#include "../includes.hpp"

#include <sdbus-c++/sdbus-c++.h>

struct SDBusSession {
    SDBusSession();
    ~SDBusSession();
    std::unique_ptr<sdbus::IObject> object;
    sdbus::ObjectPath               handle;
    // The frontend connection that created it; only it may drive or close it.
    std::string                     owner;
    std::function<void()>           onDestroy;
};

struct SDBusRequest {
    std::unique_ptr<sdbus::IObject> object;
    sdbus::ObjectPath               handle;
    std::string                     owner;
    std::function<void()>           onDestroy;
};

std::unique_ptr<SDBusSession> createDBusSession(sdbus::ObjectPath handle);
std::unique_ptr<SDBusRequest> createDBusRequest(sdbus::ObjectPath handle);
// Tells the frontend that every open session has ended and unexports them.
void                          closeAllDBusSessions();
void closeDBusSession(SDBusSession& session, bool notify);
