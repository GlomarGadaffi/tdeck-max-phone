#ifndef THREECX_ANCHOR_LOGIC_HPP
#define THREECX_ANCHOR_LOGIC_HPP

// ── 3CX / Telephony Anchor Logic (Ported directly from drawbridge) ──────────────
// Pure C++17 URL builders, base64url JWT lifetime decoder, and WebSocket entity-path tokenizer.

#include <cstdint>
#include <string>
#include <vector>

namespace threecx
{

inline constexpr int64_t kTokenFallbackLifetimeUs = 50LL * 60 * 1000000; // 50 mins

// base64url decode for JWT payload
inline bool base64UrlDecode(const std::string& in, std::vector<uint8_t>& out)
{
	auto val = [](char c) -> int {
		if (c >= 'A' && c <= 'Z') return c - 'A';
		if (c >= 'a' && c <= 'z') return c - 'a' + 26;
		if (c >= '0' && c <= '9') return c - '0' + 52;
		if (c == '-' || c == '+') return 62;
		if (c == '_' || c == '/') return 63;
		return -1;
	};

	uint32_t buf = 0;
	int bits = 0;
	for (char c : in)
	{
		if (c == '=') break;
		int v = val(c);
		if (v < 0) return false;
		buf = (buf << 6) | static_cast<uint32_t>(v);
		bits += 6;
		if (bits >= 8)
		{
			bits -= 8;
			out.push_back(static_cast<uint8_t>((buf >> bits) & 0xFF));
		}
	}
	return true;
}

// Scan flat JSON object for numeric field
inline bool scanJsonNumber(const std::string& json, const std::string& key, int64_t& out)
{
	const std::string needle = "\"" + key + "\"";
	size_t pos = 0;
	while ((pos = json.find(needle, pos)) != std::string::npos)
	{
		size_t i = pos + needle.size();
		while (i < json.size() && (json[i] == ' ' || json[i] == '\t')) ++i;
		if (i >= json.size() || json[i] != ':')
		{
			pos += needle.size();
			continue;
		}
		++i;
		while (i < json.size() && (json[i] == ' ' || json[i] == '\t')) ++i;
		if (i >= json.size()) return false;

		bool neg = false;
		if (json[i] == '-') { neg = true; ++i; }
		if (i >= json.size() || json[i] < '0' || json[i] > '9') return false;

		int64_t v = 0;
		while (i < json.size() && json[i] >= '0' && json[i] <= '9')
		{
			v = v * 10 + (json[i] - '0');
			++i;
		}
		out = neg ? -v : v;
		return true;
	}
	return false;
}

// Decode JWT lifetime (exp - iat) in microseconds
inline int64_t decodeJwtLifetimeUs(const std::string& jwt)
{
	size_t firstDot = jwt.find('.');
	if (firstDot == std::string::npos) return kTokenFallbackLifetimeUs;
	size_t secondDot = jwt.find('.', firstDot + 1);
	if (secondDot == std::string::npos) return kTokenFallbackLifetimeUs;

	std::string payload = jwt.substr(firstDot + 1, secondDot - firstDot - 1);
	if (payload.empty()) return kTokenFallbackLifetimeUs;

	std::vector<uint8_t> decoded;
	if (!base64UrlDecode(payload, decoded) || decoded.empty()) return kTokenFallbackLifetimeUs;

	std::string json(decoded.begin(), decoded.end());
	int64_t exp = 0, iat = 0;
	if (!scanJsonNumber(json, "exp", exp) || !scanJsonNumber(json, "iat", iat)) return kTokenFallbackLifetimeUs;

	int64_t span = exp - iat;
	if (span > 0 && span < 86400) return span * 1000000;
	return kTokenFallbackLifetimeUs;
}

// Split entity path on '/'
inline std::vector<std::string> splitEntityPath(const std::string& entity)
{
	std::vector<std::string> tokens;
	std::string item;
	for (char c : entity)
	{
		if (c == '/')
		{
			if (!item.empty()) tokens.push_back(item);
			item.clear();
		}
		else
		{
			item.push_back(c);
		}
	}
	if (!item.empty()) tokens.push_back(item);
	return tokens;
}

struct ParticipantEntity
{
	bool valid = false;
	std::string dn;
	std::string participantId;
};

inline ParticipantEntity parseParticipantEntity(const std::string& entity)
{
	ParticipantEntity e;
	std::vector<std::string> t = splitEntityPath(entity);
	if (t.size() == 4 && t[0] == "callcontrol" && t[2] == "participants")
	{
		e.valid = true;
		e.dn = t[1];
		e.participantId = t[3];
	}
	return e;
}

// Call-control URL builders matching drawbridge
inline std::string tokenUrl(const std::string& baseUrl) { return baseUrl + "/connect/token"; }
inline std::string participantsUrl(const std::string& baseUrl, const std::string& dn) { return baseUrl + "/callcontrol/" + dn + "/participants"; }
inline std::string devicesUrl(const std::string& baseUrl, const std::string& dn) { return baseUrl + "/callcontrol/" + dn + "/devices"; }
inline std::string legacyMakeCallUrl(const std::string& baseUrl, const std::string& dn) { return baseUrl + "/callcontrol/" + dn + "/makecall"; }
inline std::string participantActionUrl(const std::string& baseUrl, const std::string& dn, const std::string& participantId, const std::string& action) {
	std::string u = baseUrl + "/callcontrol/" + dn + "/participants/" + participantId;
	if (!action.empty()) u += "/" + action;
	return u;
}
inline std::string controlWsUrl(const std::string& baseUrl) {
	if (baseUrl.rfind("https://", 0) == 0) return "wss://" + baseUrl.substr(8) + "/callcontrol/ws";
	if (baseUrl.rfind("http://", 0) == 0) return "ws://" + baseUrl.substr(7) + "/callcontrol/ws";
	return "wss://" + baseUrl + "/callcontrol/ws";
}

} // namespace threecx

#endif // THREECX_ANCHOR_LOGIC_HPP
