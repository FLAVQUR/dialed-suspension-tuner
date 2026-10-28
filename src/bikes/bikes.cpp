#include "bikes.h"
#include "storage/storage.h"
#include "system/settings.h"
#include <SD.h>
#include <ArduinoJson.h>

namespace {

// Bike names are free text (spaces, punctuation) but filenames need to
// stay filesystem-safe — the display name always lives inside the JSON
// itself ("name" field), so this only ever affects the path on disk.
String pathFor(const String& name) {
    String safe = name;

    for (size_t i = 0; i < safe.length(); i++) {
        char c = safe[i];
        if (!isalnum((unsigned char)c) && c != ' ' && c != '-' && c != '_')
            safe.setCharAt(i, '_');
    }

    return "/bikes/" + safe + ".json";
}

void writeProfile(JsonDocument& doc, const String& name) {
    doc["name"] = name;
    doc["full_suspension"] = settings.fullSuspension;
    doc["wheel_size"] = settings.wheelSize;
    doc["rear_wheel_travel_mm"] = serialized(String(settings.rearWheelTravelMM, 1));

    JsonObject front = doc["front_suspension"].to<JsonObject>();
    front["model"] = settings.frontModel;
    front["lsc"] = settings.frontLSC;
    front["hsc"] = settings.frontHSC;
    front["lsr"] = settings.frontLSR;
    front["hsr"] = settings.frontHSR;
    front["volume_tokens"] = settings.frontVolumeTokens;
    front["pressure_psi"] = serialized(String(settings.frontPressurePsi, 0));
    front["sag_pct"] = serialized(String(settings.frontSagPct, 1));
    front["max_travel_mm"] = serialized(String(settings.frontMaxTravelMM, 1));
    front["has_lsc"] = settings.frontHasLSC;
    front["has_hsc"] = settings.frontHasHSC;
    front["has_lsr"] = settings.frontHasLSR;
    front["has_hsr"] = settings.frontHasHSR;

    JsonObject rear = doc["rear_suspension"].to<JsonObject>();
    rear["model"] = settings.rearModel;
    rear["lsc"] = settings.rearLSC;
    rear["hsc"] = settings.rearHSC;
    rear["lsr"] = settings.rearLSR;
    rear["hsr"] = settings.rearHSR;
    rear["volume_tokens"] = settings.rearVolumeTokens;
    rear["pressure_psi"] = serialized(String(settings.rearPressurePsi, 0));
    rear["sag_pct"] = serialized(String(settings.rearSagPct, 1));
    rear["stroke_mm"] = serialized(String(settings.rearStrokeMM, 1));
    rear["has_lsc"] = settings.rearHasLSC;
    rear["has_hsc"] = settings.rearHasHSC;
    rear["has_lsr"] = settings.rearHasLSR;
    rear["has_hsr"] = settings.rearHasHSR;
}

} // namespace

std::vector<String> BikeProfiles::list() {
    std::vector<String> names;

    File dir = SD.open("/bikes");
    if (!dir)
        return names;

    while (true) {
        File f = dir.openNextFile();
        if (!f) break;

        String fname = f.name();
        bool isJson = fname.endsWith(".json") || fname.endsWith(".JSON");
        f.close();

        if (!isJson) continue;

        String fullPath = fname.startsWith("/") ? fname : ("/bikes/" + fname);
        File pf = storageGet(fullPath.c_str());
        if (!pf) continue;

        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, pf);
        pf.close();

        if (!err) {
            const char* n = doc["name"] | "";
            if (strlen(n) > 0) names.push_back(String(n));
        }
    }

    dir.close();
    return names;
}

bool BikeProfiles::exists(const String& name) {
    return name.length() > 0 && storageExists(pathFor(name).c_str());
}

bool BikeProfiles::nameTaken(const String& name, const String& current) {
    // FAT doesn't tell upper case from lower, and pathFor() folds
    // punctuation, so different names can be one and the same file.
    return exists(name) && !pathFor(name).equalsIgnoreCase(pathFor(current));
}

String BikeProfiles::read(const String& name) {
    String json;
    if (!exists(name)) return json;

    File f = storageGet(pathFor(name).c_str());
    if (!f) return json;

    json = f.readString();
    f.close();
    return json;
}

bool BikeProfiles::syncActive() {
    if (!exists(settings.bikeName)) return true;

    return save(settings.bikeName);
}

bool BikeProfiles::activate(const String& name) {
    File f = storageGet(pathFor(name).c_str());
    if (!f) return false;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) return false;

    settings.bikeName = doc["name"] | name;
    settings.fullSuspension = doc["full_suspension"] | true;
    settings.wheelSize = doc["wheel_size"] | String("29");
    settings.rearWheelTravelMM = doc["rear_wheel_travel_mm"] | 0.0f;

    JsonObject front = doc["front_suspension"];
    // Absent in profiles saved before 2026-10-28: no model, as before.
    settings.frontModel = front["model"] | "";
    settings.frontLSC = front["lsc"] | 0;
    settings.frontHSC = front["hsc"] | 0;
    settings.frontLSR = front["lsr"] | 0;
    settings.frontHSR = front["hsr"] | 0;
    settings.frontVolumeTokens = front["volume_tokens"] | 0;
    settings.frontPressurePsi = front["pressure_psi"] | 0.0f;
    settings.frontSagPct = front["sag_pct"] | 0.0f;
    // Absent in profiles saved before this field existed — comes back as
    // 0.0f via this same fallback, same as every other field here. A rider
    // switching to one of those older profiles needs to re-enter/recapture
    // front travel once; see suspensionGetMaxTravelMM().
    settings.frontMaxTravelMM = front["max_travel_mm"] | 0.0f;
    // Absent in profiles saved before this field existed too — falls back
    // to the same "has LSC/LSR, no HSC/HSR" assumption as a factory-fresh
    // settingsLoadDefaults(), the single-adjuster case most dampers are.
    settings.frontHasLSC = front["has_lsc"] | true;
    settings.frontHasHSC = front["has_hsc"] | false;
    settings.frontHasLSR = front["has_lsr"] | true;
    settings.frontHasHSR = front["has_hsr"] | false;

    JsonObject rear = doc["rear_suspension"];
    settings.rearModel = rear["model"] | "";
    settings.rearLSC = rear["lsc"] | 0;
    settings.rearHSC = rear["hsc"] | 0;
    settings.rearLSR = rear["lsr"] | 0;
    settings.rearHSR = rear["hsr"] | 0;
    settings.rearVolumeTokens = rear["volume_tokens"] | 0;
    settings.rearPressurePsi = rear["pressure_psi"] | 0.0f;
    settings.rearSagPct = rear["sag_pct"] | 0.0f;
    settings.rearStrokeMM = rear["stroke_mm"] | 0.0f;
    settings.rearHasLSC = rear["has_lsc"] | true;
    settings.rearHasHSC = rear["has_hsc"] | false;
    settings.rearHasLSR = rear["has_lsr"] | true;
    settings.rearHasHSR = rear["has_hsr"] | false;

    settingsSave();
    return true;
}

bool BikeProfiles::save(const String& name) {
    if (name.length() == 0) return false;

    SD.mkdir("/bikes");

    File f = storageOpen(pathFor(name).c_str());
    if (!f) return false;

    JsonDocument doc;
    writeProfile(doc, name);

    serializeJson(doc, f);
    storageClose(f);
    return true;
}

bool BikeProfiles::remove(const String& name) {
    return SD.remove(pathFor(name));
}

bool BikeProfiles::rename(const String& oldName, const String& newName) {
    if (newName.length() == 0) return false;

    // Never onto another bike's profile: opening it for writing would
    // quietly replace that bike's setup with this one's.
    if (nameTaken(newName, oldName)) return false;

    String oldPath = pathFor(oldName);
    String newPath = pathFor(newName);

    File f = storageGet(oldPath.c_str());
    if (!f) return false;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) return false;

    doc["name"] = newName;

    File nf = storageOpen(newPath.c_str());
    if (!nf) return false;

    serializeJson(doc, nf);
    storageClose(nf);

    // Both names can be the same file (see nameTaken()), which was then
    // just rewritten in place — removing "the old one" would remove it.
    if (!oldPath.equalsIgnoreCase(newPath)) SD.remove(oldPath);

    if (settings.bikeName == oldName) {
        settings.bikeName = newName;
        settingsSave();
    }

    return true;
}
