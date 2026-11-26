#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// A screen-cast or remote-desktop permission the user granted once, redeemable
// by the app that received its token.
struct SScreencastGrant {
    enum eKind : uint8_t {
        KIND_OUTPUT,
        KIND_REGION,
        KIND_WINDOW,
    };

    // Names the grant in the store without revealing its token.
    std::string id;
    // The secret that restore_data carries back.
    std::string token;
    std::string appID;
    eKind       kind = KIND_OUTPUT;
    std::string output;
    // The monitor's make, model and serial as the compositor describes it.
    std::string outputIdentity;
    // Only session grants can match an output by its Wayland registry global.
    uint32_t    outputGlobal = 0;
    uint32_t    x = 0, y = 0, w = 0, h = 0;
    uint64_t    windowHandle = 0;
    std::string windowClass;
    bool        withCursor = false;
    // Remote-desktop input devices allowed without asking again.
    uint32_t devices    = 0;
    bool     persistent = false;
    uint64_t issued     = 0;
};

// Session grants live in memory; persistent ones in a user-private file that
// Settings may edit to revoke them. Every grant's token works once: the
// backend issues a fresh one each time it starts a session from a grant.
class CGrantStore {
  public:
    // An empty path keeps every grant in memory.
    explicit CGrantStore(std::string path);

    static std::string defaultPath();

    // Persists only grants that can be matched again exactly: outputs and
    // regions with a known monitor identity, from a non-empty app id.
    static bool persistable(const SScreencastGrant& grant);

    // Whether the output now using the grant's connector, if any, is the monitor it was granted for.
    static bool sameMonitor(const SScreencastGrant& grant, const std::string* currentIdentity);

    // Records the grant under a new token and returns it, replacing a persistent
    // grant for the same app and source. A persistent grant that cannot be saved
    // becomes a session grant.
    SScreencastGrant issue(SScreencastGrant grant);

    // Removes and returns the grant with this token when it belongs to appID.
    std::optional<SScreencastGrant> redeem(const std::string& token, const std::string& appID);

    std::vector<SScreencastGrant>   persisted() const;

    static constexpr size_t         MAX_SESSION_GRANTS    = 32;
    static constexpr size_t         MAX_PERSISTENT_GRANTS = 64;

  private:
    std::string                   m_path;
    std::vector<SScreencastGrant> m_session;
};

std::string                   serializeGrants(const std::vector<SScreencastGrant>& grants);
std::vector<SScreencastGrant> parseGrants(const std::string& text);
