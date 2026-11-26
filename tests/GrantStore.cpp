#include "../src/shared/GrantStore.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

static int failures = 0;

#define CHECK(cond)                                                                                                                                                                \
    do {                                                                                                                                                                           \
        if (!(cond)) {                                                                                                                                                             \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond);                                                                                          \
            ++failures;                                                                                                                                                            \
        }                                                                                                                                                                          \
    } while (0)

static SScreencastGrant screen(const std::string& app, bool persistent) {
    return SScreencastGrant{.appID = app, .kind = SScreencastGrant::KIND_OUTPUT, .output = "DP-1", .outputIdentity = "Dell Inc. U2720Q 7ZX1 (DP-1)", .persistent = persistent};
}

static std::string slurp(const fs::path& path) {
    std::ifstream     in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static void test(const char* name, const std::function<void(const fs::path&)>& body) {
    auto dir = (fs::temp_directory_path() / "grant-store-XXXXXX").string();
    if (!mkdtemp(dir.data()))
        abort();
    const auto before = failures;
    body(fs::path{dir} / "state" / "screencast-grants");
    fs::remove_all(dir);
    std::fprintf(stderr, "%s %s\n", failures == before ? "ok  " : "FAIL", name);
}

int main() {
    test("persistent grant survives a restart", [](const fs::path& path) {
        std::string token;
        {
            CGrantStore store{path};
            const auto  issued = store.issue(screen("org.obs.Studio", true));
            CHECK(issued.persistent);
            token = issued.token;
        }
        CGrantStore restarted{path};
        const auto  grant = restarted.redeem(token, "org.obs.Studio");
        CHECK(grant && grant->output == "DP-1" && grant->persistent);
    });

    test("persistent grant tokens work once", [](const fs::path& path) {
        CGrantStore store{path};
        const auto  token = store.issue(screen("org.obs.Studio", true)).token;
        CHECK(store.redeem(token, "org.obs.Studio"));
        CHECK(!store.redeem(token, "org.obs.Studio"));
        CHECK(!CGrantStore{path}.redeem(token, "org.obs.Studio"));
    });

    test("reissuing a restored grant keeps its id and rotates its token", [](const fs::path& path) {
        CGrantStore store{path};
        const auto  first = store.issue(screen("org.obs.Studio", true));
        auto        grant = *store.redeem(first.token, "org.obs.Studio");
        const auto  again = store.issue(grant);
        CHECK(again.id == first.id && again.token != first.token);
        CHECK(store.persisted().size() == 1);
    });

    test("a new grant for the same app and source replaces the earlier one", [](const fs::path& path) {
        CGrantStore store{path};
        const auto  first  = store.issue(screen("org.obs.Studio", true));
        auto        region = screen("org.obs.Studio", true);
        region.kind        = SScreencastGrant::KIND_REGION;
        region.w = region.h = 100;
        store.issue(region);
        store.issue(screen("com.example.Other", true));
        const auto second = store.issue(screen("org.obs.Studio", true));
        CHECK(second.id != first.id);
        CHECK(store.persisted().size() == 3);
        CHECK(!store.redeem(first.token, "org.obs.Studio"));
        CHECK(store.redeem(second.token, "org.obs.Studio"));
    });

    test("session grant is not persisted", [](const fs::path& path) {
        CGrantStore store{path};
        const auto  issued = store.issue(screen("org.obs.Studio", false));
        CHECK(!issued.persistent);
        CHECK(store.persisted().empty());
        CHECK(!fs::exists(path) || slurp(path).find(issued.token) == std::string::npos);
        CHECK(!CGrantStore{path}.redeem(issued.token, "org.obs.Studio"));
        CHECK(store.redeem(issued.token, "org.obs.Studio"));
        CHECK(!store.redeem(issued.token, "org.obs.Studio"));
    });

    test("session grant drops remembered devices", [](const fs::path& path) {
        auto grant    = screen("org.obs.Studio", false);
        grant.devices = 3;
        CGrantStore store{path};
        CHECK(store.issue(grant).devices == 0);
    });

    test("sources without a persistent identity stay session grants", [](const fs::path& path) {
        CGrantStore store{path};
        auto        window = SScreencastGrant{.appID = "org.obs.Studio", .kind = SScreencastGrant::KIND_WINDOW, .windowClass = "firefox", .persistent = true};
        CHECK(!store.issue(window).persistent);
        CHECK(!store.issue(screen("", true)).persistent);
        auto unknownMonitor           = screen("org.obs.Studio", true);
        unknownMonitor.outputIdentity = "";
        CHECK(!store.issue(unknownMonitor).persistent);
        unknownMonitor.outputIdentity = "unidentified output";
        CHECK(!store.issue(unknownMonitor).persistent);
        CHECK(parseGrants(serializeGrants({unknownMonitor})).empty());
        CHECK(store.persisted().empty());
    });

    test("foreign app id is refused and spends the token", [](const fs::path& path) {
        CGrantStore store{path};
        const auto  persisted = store.issue(screen("org.obs.Studio", true)).token;
        const auto  session   = store.issue(screen("org.obs.Studio", false)).token;
        CHECK(!store.redeem(persisted, "com.example.Spy"));
        CHECK(!store.redeem(session, "com.example.Spy"));
        CHECK(!store.redeem(persisted, "org.obs.Studio"));
        CHECK(!store.redeem(session, "org.obs.Studio"));
    });

    test("forged token is refused", [](const fs::path& path) {
        CGrantStore store{path};
        store.issue(screen("org.obs.Studio", true));
        store.issue(screen("org.obs.Studio", false));
        CHECK(!store.redeem("todo", "org.obs.Studio"));
        CHECK(!store.redeem("", "org.obs.Studio"));
        CHECK(!store.redeem(std::string(64, '0'), "org.obs.Studio"));
        CHECK(store.persisted().size() == 1);
    });

    test("revoking in the file takes effect without a restart", [](const fs::path& path) {
        CGrantStore store{path};
        const auto  issued = store.issue(screen("org.obs.Studio", true));
        auto        grants = parseGrants(slurp(path));
        CHECK(grants.size() == 1 && grants[0].id == issued.id);
        std::erase_if(grants, [&](const auto& g) { return g.id == issued.id; });
        std::ofstream(path, std::ios::trunc) << serializeGrants(grants);
        CHECK(!store.redeem(issued.token, "org.obs.Studio"));
    });

    test("grant file and directory are private", [](const fs::path& path) {
        CGrantStore store{path};
        store.issue(screen("org.obs.Studio", true));
        struct stat st{};
        CHECK(stat(path.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
        CHECK(stat(path.parent_path().c_str(), &st) == 0 && (st.st_mode & 0777) == 0700);
    });

    test("a grant file others can read or write is ignored", [](const fs::path& path) {
        CGrantStore store{path};
        const auto  token = store.issue(screen("org.obs.Studio", true)).token;
        chmod(path.c_str(), 0644);
        CHECK(!store.redeem(token, "org.obs.Studio"));
        CHECK(store.persisted().empty());
        CHECK(!store.issue(screen("org.obs.Studio", true)).persistent);
    });

    test("a symlinked grant file is ignored", [](const fs::path& path) {
        CGrantStore store{path};
        const auto  token = store.issue(screen("org.obs.Studio", true)).token;
        fs::rename(path, path.string() + ".real");
        fs::create_symlink(path.string() + ".real", path);
        CHECK(!store.redeem(token, "org.obs.Studio"));
    });

    test("a monitor with another identity on the same connector does not match", [](const fs::path&) {
        const auto        grant = screen("org.obs.Studio", true);
        const std::string same  = "Dell Inc. U2720Q 7ZX1 (DP-1)";
        const std::string other = "LG Electronics 27GL850 9AB2 (DP-1)";
        const std::string blank;
        CHECK(CGrantStore::sameMonitor(grant, &same));
        CHECK(!CGrantStore::sameMonitor(grant, &other));
        CHECK(!CGrantStore::sameMonitor(grant, &blank));
        CHECK(!CGrantStore::sameMonitor(grant, nullptr));
    });

    test("fields with separators round-trip", [](const fs::path&) {
        auto grant           = screen("org.obs.Studio", true);
        grant.id             = "a1";
        grant.token          = "t";
        grant.outputIdentity = "Odd\tname\n100%";
        grant.kind           = SScreencastGrant::KIND_REGION;
        grant.w              = 10;
        grant.h              = 20;
        grant.devices        = 3;
        const auto parsed    = parseGrants(serializeGrants({grant}));
        CHECK(parsed.size() == 1 && parsed[0].outputIdentity == grant.outputIdentity && parsed[0].kind == SScreencastGrant::KIND_REGION && parsed[0].devices == 3);
        CHECK(parseGrants("enzim-screencast-grants 2\n" + serializeGrants({grant}).substr(26)).empty());
    });

    test("the oldest persisted grants make room for new ones", [](const fs::path& path) {
        CGrantStore store{path};
        const auto  oldest = store.issue(screen("org.obs.Studio", true)).token;
        for (size_t i = 0; i < CGrantStore::MAX_PERSISTENT_GRANTS; ++i)
            store.issue(screen("org.example.App" + std::to_string(i), true));
        CHECK(store.persisted().size() == CGrantStore::MAX_PERSISTENT_GRANTS);
        CHECK(!store.redeem(oldest, "org.obs.Studio"));
    });

    return failures ? 1 : 0;
}
