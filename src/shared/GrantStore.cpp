#include "GrantStore.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <ctime>
#include <fcntl.h>
#include <format>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

static constexpr const char* HEADER = "enzim-screencast-grants 1";

static std::string           randomHex(size_t bytes) {
    std::string raw(bytes, '\0');
    size_t      filled = 0;
    while (filled < bytes) {
        const auto got = getrandom(raw.data() + filled, bytes - filled, 0);
        if (got < 0) {
            if (errno == EINTR)
                continue;
            abort();
        }
        filled += got;
    }
    std::string hex;
    for (unsigned char c : raw)
        hex += std::format("{:02x}", c);
    return hex;
}

static std::string escape(const std::string& field) {
    std::string out;
    for (unsigned char c : field) {
        if (c == '%' || c == '\t' || c == '\n' || c == '\r' || c < 0x20)
            out += std::format("%{:02X}", c);
        else
            out += (char)c;
    }
    return out;
}

static std::optional<std::string> unescape(std::string_view field) {
    std::string out;
    for (size_t i = 0; i < field.size(); ++i) {
        if (field[i] != '%') {
            out += field[i];
            continue;
        }
        unsigned value = 0;
        if (i + 2 >= field.size())
            return std::nullopt;
        const auto res = std::from_chars(field.data() + i + 1, field.data() + i + 3, value, 16);
        if (res.ec != std::errc{} || res.ptr != field.data() + i + 3)
            return std::nullopt;
        out += (char)value;
        i += 2;
    }
    return out;
}

template <typename T>
static bool number(std::string_view text, T& out) {
    const auto res = std::from_chars(text.data(), text.data() + text.size(), out);
    return res.ec == std::errc{} && res.ptr == text.data() + text.size();
}

std::string serializeGrants(const std::vector<SScreencastGrant>& grants) {
    std::string text = std::string{HEADER} + "\n";
    for (const auto& g : grants) {
        text += std::format("grant\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\n", escape(g.id), escape(g.token), escape(g.appID),
                            g.kind == SScreencastGrant::KIND_REGION ? "region" : "output", escape(g.output), escape(g.outputIdentity), g.x, g.y, g.w, g.h, g.withCursor ? 1 : 0,
                            g.devices, g.issued);
    }
    return text;
}

std::vector<SScreencastGrant> parseGrants(const std::string& text) {
    std::vector<SScreencastGrant> grants;
    size_t                        start = 0;
    bool                          first = true;
    while (start < text.size()) {
        auto end = text.find('\n', start);
        if (end == std::string::npos)
            end = text.size();
        const std::string_view line{text.data() + start, end - start};
        start = end + 1;
        if (first) {
            first = false;
            if (line != HEADER)
                return {};
            continue;
        }
        std::vector<std::string_view> fields;
        size_t                        at = 0;
        while (true) {
            const auto tab = line.find('\t', at);
            fields.push_back(line.substr(at, tab == std::string_view::npos ? std::string_view::npos : tab - at));
            if (tab == std::string_view::npos)
                break;
            at = tab + 1;
        }
        if (fields.size() != 14 || fields[0] != "grant")
            continue;
        SScreencastGrant g;
        g.persistent = true;
        auto id      = unescape(fields[1]);
        auto token   = unescape(fields[2]);
        auto app     = unescape(fields[3]);
        auto output  = unescape(fields[5]);
        auto ident   = unescape(fields[6]);
        if (!id || !token || !app || !output || !ident)
            continue;
        if (fields[4] == "output")
            g.kind = SScreencastGrant::KIND_OUTPUT;
        else if (fields[4] == "region")
            g.kind = SScreencastGrant::KIND_REGION;
        else
            continue;
        unsigned cursor = 0;
        if (!number(fields[7], g.x) || !number(fields[8], g.y) || !number(fields[9], g.w) || !number(fields[10], g.h) || !number(fields[11], cursor) ||
            !number(fields[12], g.devices) || !number(fields[13], g.issued))
            continue;
        g.id             = *id;
        g.token          = *token;
        g.appID          = *app;
        g.output         = *output;
        g.outputIdentity = *ident;
        g.withCursor     = cursor != 0;
        if (CGrantStore::persistable(g))
            grants.push_back(std::move(g));
    }
    return grants;
}

namespace {
    // Serializes read-modify-write cycles with Settings and other backend instances.
    class CLockedFile {
      public:
        explicit CLockedFile(const std::string& path) : m_path(path) {
            if (path.empty() || !ensureParent())
                return;
            m_lock = open((path + ".lock").c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
            if (m_lock >= 0 && flock(m_lock, LOCK_EX) != 0) {
                close(m_lock);
                m_lock = -1;
            }
        }
        ~CLockedFile() {
            if (m_lock >= 0)
                close(m_lock);
        }

        bool locked() const {
            return m_lock >= 0;
        }

        // nullopt when the file exists but cannot be trusted or read.
        std::optional<std::vector<SScreencastGrant>> read() const {
            const int fd = open(m_path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
            if (fd < 0)
                return errno == ENOENT ? std::optional{std::vector<SScreencastGrant>{}} : std::nullopt;
            struct stat st{};
            std::string text;
            // Another user able to write it could plant grants.
            if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 077) != 0 || st.st_size > (1 << 20)) {
                close(fd);
                return std::nullopt;
            }
            char    buf[4096];
            ssize_t n;
            while ((n = ::read(fd, buf, sizeof(buf))) > 0)
                text.append(buf, n);
            close(fd);
            if (n < 0)
                return std::nullopt;
            if (!text.empty() && !text.starts_with(std::string{HEADER} + "\n"))
                return std::nullopt;
            return parseGrants(text);
        }

        bool write(const std::vector<SScreencastGrant>& grants) const {
            std::string tmp = m_path + ".XXXXXX";
            const int   fd  = mkostemp(tmp.data(), O_CLOEXEC);
            if (fd < 0)
                return false;
            const auto text = serializeGrants(grants);
            bool       ok   = fchmod(fd, 0600) == 0;
            size_t     done = 0;
            while (ok && done < text.size()) {
                const auto n = ::write(fd, text.data() + done, text.size() - done);
                if (n < 0 && errno == EINTR)
                    continue;
                ok = n > 0;
                done += n > 0 ? n : 0;
            }
            ok = ok && fsync(fd) == 0;
            ok = close(fd) == 0 && ok;
            if (!ok || rename(tmp.c_str(), m_path.c_str()) != 0) {
                unlink(tmp.c_str());
                return false;
            }
            return true;
        }

      private:
        bool ensureParent() const {
            const auto slash = m_path.rfind('/');
            if (slash == std::string::npos || slash == 0)
                return false;
            const auto dir = m_path.substr(0, slash);
            for (size_t at = 1; at <= dir.size(); ++at) {
                if (at != dir.size() && dir[at] != '/')
                    continue;
                if (mkdir(dir.substr(0, at).c_str(), 0700) != 0 && errno != EEXIST)
                    return false;
            }
            struct stat st{};
            return lstat(dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == getuid() && (st.st_mode & 022) == 0;
        }

        std::string m_path;
        int         m_lock = -1;
    };
}

CGrantStore::CGrantStore(std::string path) : m_path(std::move(path)) {}

std::string CGrantStore::defaultPath() {
    const char* state = getenv("XDG_STATE_HOME");
    const char* home  = getenv("HOME");
    std::string base;
    if (state && state[0] == '/')
        base = state;
    else if (home && home[0] == '/')
        base = std::string{home} + "/.local/state";
    else
        return "";
    return base + "/xdg-desktop-portal-hyprland/screencast-grants";
}

bool CGrantStore::persistable(const SScreencastGrant& grant) {
    if (grant.appID.empty() || grant.output.empty() || grant.outputIdentity.empty() || grant.outputIdentity == "unidentified output")
        return false;
    if (grant.kind == SScreencastGrant::KIND_REGION)
        return grant.w > 0 && grant.h > 0;
    return grant.kind == SScreencastGrant::KIND_OUTPUT;
}

bool CGrantStore::sameMonitor(const SScreencastGrant& grant, const std::string* currentIdentity) {
    return currentIdentity && !currentIdentity->empty() && *currentIdentity == grant.outputIdentity;
}

SScreencastGrant CGrantStore::issue(SScreencastGrant grant) {
    grant.token  = randomHex(32);
    grant.issued = grant.issued ? grant.issued : (uint64_t)time(nullptr);
    if (grant.id.empty())
        grant.id = randomHex(8);

    if (grant.persistent && persistable(grant)) {
        CLockedFile file{m_path};
        auto        grants = file.locked() ? file.read() : std::nullopt;
        if (grants) {
            // One grant per app and source: a new choice replaces the earlier one and its token.
            std::erase_if(*grants, [&](const auto& g) {
                return g.id == grant.id ||
                    (g.appID == grant.appID && g.kind == grant.kind && g.output == grant.output && g.outputIdentity == grant.outputIdentity && g.x == grant.x && g.y == grant.y &&
                     g.w == grant.w && g.h == grant.h);
            });
            grants->push_back(grant);
            if (grants->size() > MAX_PERSISTENT_GRANTS)
                grants->erase(grants->begin(), grants->end() - MAX_PERSISTENT_GRANTS);
            if (file.write(*grants))
                return grant;
        }
    }

    grant.persistent = false;
    grant.devices    = 0;
    if (m_session.size() >= MAX_SESSION_GRANTS)
        m_session.erase(m_session.begin());
    m_session.push_back(grant);
    return grant;
}

std::optional<SScreencastGrant> CGrantStore::redeem(const std::string& token, const std::string& appID) {
    if (token.empty())
        return std::nullopt;

    // A token that reaches the wrong app is spent all the same.
    const auto session = std::ranges::find_if(m_session, [&](const auto& g) { return g.token == token; });
    if (session != m_session.end()) {
        auto grant = std::move(*session);
        m_session.erase(session);
        return grant.appID == appID ? std::optional{grant} : std::nullopt;
    }

    CLockedFile file{m_path};
    auto        grants = file.locked() ? file.read() : std::nullopt;
    if (!grants)
        return std::nullopt;
    const auto it = std::ranges::find_if(*grants, [&](const auto& g) { return g.token == token; });
    if (it == grants->end())
        return std::nullopt;
    auto grant = std::move(*it);
    grants->erase(it);
    if (!file.write(*grants))
        return std::nullopt;
    return grant.appID == appID ? std::optional{grant} : std::nullopt;
}

std::vector<SScreencastGrant> CGrantStore::persisted() const {
    CLockedFile file{m_path};
    auto        grants = file.locked() ? file.read() : std::nullopt;
    return grants.value_or(std::vector<SScreencastGrant>{});
}
