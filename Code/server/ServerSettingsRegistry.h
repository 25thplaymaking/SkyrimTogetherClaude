// Copyright (C) 2026 TiltedPhoques SRL.
// For licensing information see LICENSE at the root of this distribution.
#pragma once

#include <Setting.h>

// Settings read from more than one translation unit are declared here and
// defined exactly once (in GameServer.cpp).
//
// Console::Setting registers itself into a global intrusive list in its
// constructor (Setting.h, SettingBase::SettingBase). Defining the same setting
// name twice therefore creates two live registry entries -- even when one is in
// an anonymous namespace -- and the console's /set resolves to whichever it
// finds first while the consumer keeps reading the other. The symptom is a
// setting that silently does nothing.
//
// Declare here; define once; never re-declare locally.

extern Console::Setting<bool> bAutoPartyJoin;
