#pragma once

#include <Arduino.h>
#include <vector>

// A small "garage" of saved bike setups, one JSON file per bike under
// /bikes/ on the SD card — sibling to how run metadata is stored.
// Activating a bike overwrites the live settings (name, wheel/travel,
// full-suspension flag, and both sides' LSC/HSC/LSR/HSR/volume tokens/
// pressure) and persists them to NVS exactly like a normal Settings save,
// so nothing else in the firmware needs to know a multi-bike garage
// exists at all — it only ever sees "the current settings".
//
// Deliberately NOT included here: suspension calibration (zero/max
// potentiometer points). That's tied to how the sensor happens to be
// physically mounted, not to which bike is logically "active" — the
// user explicitly asked for these to stay independent.
//
// The active bike is the profile saved under settings.bikeName, when there
// is one, and that profile is kept equal to the live settings: whatever
// changes a setting a profile holds calls syncActive() afterwards. It used
// to be written only by an explicit "Save", which is to say it was out of
// date after every edit until someone remembered to press it — and
// activating it again brought the old values back. The name is part of
// this: on a bike that has a profile, a new name renames the profile
// instead of leaving it behind under the old one (see the config handler in
// server.cpp).
namespace BikeProfiles
{
    std::vector<String> list();

    bool exists(const String& name);

    // Whether `name` is already another profile's — one that renaming
    // `current` to it would overwrite. Two names that come down to the same
    // file (see pathFor()) are the same profile, not a clash.
    bool nameTaken(const String& name, const String& current);

    // The profile as it is on the card, for inspection ("" if there is none).
    String read(const String& name);

    bool activate(const String& name);
    bool save(const String& name);
    bool remove(const String& name);
    bool rename(const String& oldName, const String& newName);

    // Rewrites the active bike's profile from the live settings. Does
    // nothing (and returns true) when the live settings aren't a saved
    // profile — a first profile is still created by save().
    bool syncActive();
}
