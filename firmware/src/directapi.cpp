/* Direct HTTPS client for api.anthropic.com/api/oauth/usage with self-managed
 * OAuth refresh. TLS is verified against the pinned root CAs in certs.h;
 * cert validation needs wall-clock time, so callers must have NTP running. */
#include "directapi.h"
#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <time.h>
#include "certs.h"
#include "config.h"

#ifndef TZ_OFFSET_SECONDS
#define TZ_OFFSET_SECONDS (-3 * 3600)
#endif

static const char *USAGE_URL = "https://api.anthropic.com/api/oauth/usage";
static const char *TOKEN_URL = "https://console.anthropic.com/v1/oauth/token";
static const char *CLIENT_ID = "9d1c250a-e61b-44d9-88ed-5944d1962f5e";

static const uint32_t POLL_MS = 120000;
static const uint32_t POLL_MAX_MS = 600000;

static Preferences prefs;
static String accessToken, refreshToken, planName;
static int64_t expiresAtMs = 0;
static uint32_t backoffMs = POLL_MS;

/* activity heuristic: the 5h window pct rising means the account is in use */
static int lastPct5h = -1;
static time_t lastRiseAt = 0;

void directapi_init() {
  prefs.begin("auth", false);
  accessToken = prefs.getString("at", "");
  refreshToken = prefs.getString("rt", "");
  expiresAtMs = prefs.getLong64("exp", 0);
  planName = prefs.getString("plan", "");
}

bool directapi_has_token() { return refreshToken.length() > 0; }

void directapi_store_tokens(const char *access, const char *refresh,
                            int64_t expMs, const char *plan) {
  accessToken = access;
  refreshToken = refresh;
  expiresAtMs = expMs;
  if (plan && plan[0]) planName = plan;
  prefs.putString("at", accessToken);
  prefs.putString("rt", refreshToken);
  prefs.putLong64("exp", expiresAtMs);
  prefs.putString("plan", planName);
  Serial.println("[direct] tokens stored in NVS");
}

uint32_t directapi_wait_ms() { return backoffMs; }

static bool time_synced() { return time(nullptr) > 1700000000; }
static int64_t now_ms() { return (int64_t)time(nullptr) * 1000; }

/* Parse "YYYY-MM-DDTHH:MM:SS(.frac)?(+00:00|Z)" (UTC) into epoch seconds. */
static time_t parse_iso_utc(const char *s) {
  int Y, M, D, h, m, sec;
  if (sscanf(s, "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &sec) != 6) return 0;
  /* days-from-civil (Howard Hinnant), valid for our date range */
  int y = Y - (M <= 2);
  int era = y / 400;
  int yoe = y - era * 400;
  int doy = (153 * (M + (M > 2 ? -3 : 9)) + 2) / 5 + D - 1;
  int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  long days = (long)era * 146097 + doe - 719468;
  return (time_t)days * 86400 + h * 3600 + m * 60 + sec;
}

static bool refresh_tokens() {
  WiFiClientSecure client;
  client.setCACert(ANTHROPIC_ROOT_CAS);
  HTTPClient http;
  http.setTimeout(15000);
  if (!http.begin(client, TOKEN_URL)) return false;
  http.addHeader("Content-Type", "application/json");
  JsonDocument body;
  body["grant_type"] = "refresh_token";
  body["refresh_token"] = refreshToken;
  body["client_id"] = CLIENT_ID;
  String payload;
  serializeJson(body, payload);
  int code = http.POST(payload);
  Serial.printf("[direct] token refresh -> %d\n", code);
  if (code != 200) { http.end(); return false; }
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getString());
  http.end();
  if (err) return false;
  const char *at = doc["access_token"] | "";
  if (!at[0]) return false;
  accessToken = at;
  const char *rt = doc["refresh_token"] | "";
  if (rt[0]) refreshToken = rt;
  long expIn = doc["expires_in"] | 3600L;
  expiresAtMs = now_ms() + (int64_t)expIn * 1000;
  prefs.putString("at", accessToken);
  prefs.putString("rt", refreshToken);
  prefs.putLong64("exp", expiresAtMs);
  return true;
}

bool directapi_fetch(BuddyData &out) {
  if (!directapi_has_token()) return false;
  if (!time_synced()) { Serial.println("[direct] waiting for NTP"); return false; }

  if (expiresAtMs && expiresAtMs < now_ms() + 120000) {
    if (!refresh_tokens()) { backoffMs = POLL_MS; return false; }
  }

  WiFiClientSecure client;
  client.setCACert(ANTHROPIC_ROOT_CAS);
  HTTPClient http;
  http.setTimeout(15000);
  if (!http.begin(client, USAGE_URL)) return false;
  http.addHeader("Authorization", String("Bearer ") + accessToken);
  http.addHeader("anthropic-beta", "oauth-2025-04-20");
  int code = http.GET();
  Serial.printf("[direct] GET usage -> %d\n", code);
  if (code == 429) {
    backoffMs = min(backoffMs * 2, POLL_MAX_MS);
    http.end();
    return false;
  }
  if (code != 200) { http.end(); backoffMs = POLL_MS; return false; }
  backoffMs = POLL_MS;

  /* only materialize the limits array to keep RAM use small */
  JsonDocument filter;
  filter["limits"] = true;
  JsonDocument doc;
  DeserializationError err =
      deserializeJson(doc, http.getString(), DeserializationOption::Filter(filter));
  http.end();
  if (err) { Serial.printf("[direct] json err: %s\n", err.c_str()); return false; }

  out = BuddyData{};
  out.companionOk = true;
  out.source = SRC_DIRECT;
  strlcpy(out.plan, planName.c_str(), sizeof(out.plan));

  time_t now = time(nullptr);
  for (JsonObject l : doc["limits"].as<JsonArray>()) {
    if (out.nLimits >= 4) break;
    LimitBar &b = out.limits[out.nLimits++];
    const char *kind = l["kind"] | "";
    if (strcmp(kind, "session") == 0) strlcpy(b.label, "5h", sizeof(b.label));
    else if (strcmp(kind, "weekly_all") == 0) strlcpy(b.label, "Semana", sizeof(b.label));
    else if (strcmp(kind, "weekly_scoped") == 0)
      strlcpy(b.label, l["scope"]["model"]["display_name"] | "Modelo", sizeof(b.label));
    else strlcpy(b.label, kind, sizeof(b.label));
    strlcpy(b.kind, kind, sizeof(b.kind));
    b.pct = l["percent"].isNull() ? -1 : (int)l["percent"];
    strlcpy(b.severity, l["severity"] | "normal", sizeof(b.severity));
    b.isActive = l["is_active"] | false;
    const char *ra = l["resets_at"] | "";
    time_t reset = ra[0] ? parse_iso_utc(ra) : 0;
    b.resetsInSec = reset > now ? (long)(reset - now) : -1;

    if (strcmp(kind, "session") == 0 && b.pct >= 0) {
      if (lastPct5h >= 0 && b.pct > lastPct5h) lastRiseAt = now;
      lastPct5h = b.pct;
    }
  }

  /* account-wide activity: usage grew recently → someone (any device) is working */
  out.active = lastRiseAt && (now - lastRiseAt) < 300;
  out.lastActivityAgoSec = lastRiseAt ? (long)(now - lastRiseAt) : -1;
  out.activeSessions = 0;
  out.tokensToday = -1; /* unavailable without the companion */

  /* Local date for the pomodoro daily counter. Apply the offset by hand on
   * the UTC epoch instead of trusting the TZ environment: a date that leaks
   * UTC here after ~21:00 ART looks like "tomorrow" and used to trigger a
   * spurious midnight rollover that zeroed the day's cycle count. */
  time_t lt = time(nullptr) + TZ_OFFSET_SECONDS;
  struct tm tmv;
  gmtime_r(&lt, &tmv);
  snprintf(out.date, sizeof(out.date), "%04d-%02d-%02d",
           tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
  return true;
}
