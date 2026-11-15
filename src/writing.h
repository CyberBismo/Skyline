// Skyline — Copyright © 2026 Yen-Zen & Cyber. All rights reserved. See LICENSE.txt.
// SkyWritingTools (plugin, off by default): select text anywhere, press your hotkey, and an AI proofreads,
// rewrites, summarises … it, right where you are. Inspired by Writing Tools by theJayTea
// (https://github.com/theJayTea/WritingTools, GPL-3.0); written anew for Skyline in C++, with its prompts as a guide.
// Works with the Gemini API (free key) or anything OpenAI-compatible (ChatGPT, OpenRouter, Ollama on this PC …).
// Nothing runs until the hotkey is pressed.
#pragma once
#include "common.h"
#include <string>
#include <vector>

namespace WritingTools {
constexpr int kHotkeyId = 0x6101;          // RegisterHotKey id on the control window

bool ApplyHotkey();                       // (re)register after a settings change; false if the keys are taken
void SuspendHotkey();                     // while a new hotkey is being recorded
void OnHotkey();                          // WM_HOTKEY

// The API key for a provider (0 Gemini, 1 OpenAI-compatible), kept encrypted by Windows for this user
std::wstring LoadKey(int provider);
bool SaveKey(int provider, const std::wstring& key);

// A small "type a value" window (for the key, the model and the address). Returns false on Cancel.
bool AskText(HWND owner, const wchar_t* title, const wchar_t* label, std::wstring& value, bool secret);

void TestConnection();                    // sends a tiny request and says how it went

// The models to pick from: the list fetched for the user's key (kept in skyline.ini), or a short built-in one
std::vector<std::wstring> ModelList(int provider);
std::wstring ModelLabel(int provider, const std::wstring& id);   // the name, plus how busy it tends to be
void FetchModels(bool quiet);             // ask the service which models this key can use (then the settings refresh)
}
