#include "TFDSettings.h"

#define NOMINMAX
#include <Windows.h>
#include <ShlObj.h>
#include <Objbase.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include <spdlog/spdlog.h>

namespace fs = std::filesystem;

namespace TFD::Settings
{
	namespace
	{
		std::atomic_bool   g_enabled{ true };
		std::atomic<float> g_defeatThresholdPct{ 30.0f };
		std::atomic<float> g_playerGuardThresholdPct{ 40.0f };
		std::atomic<float> g_playerGuardDamageScale{ 0.10f };
		std::atomic<float> g_allyDownedThresholdPct{ 20.0f };
		std::atomic<float> g_enemyDownedThresholdPct{ 10.0f };
		std::atomic<int>   g_bleedWindowSeconds{ 30 };
		std::atomic<float> g_scanRadius{ 2500.0f };
		std::atomic<float> g_sweepRadius{ 2500.0f };

		std::atomic_bool      g_hotkeyEnabled{ true };
		std::atomic_uint32_t  g_hotkeyScanCode{ 35 };   // H
		std::atomic<int>      g_hotkeyCooldownMs{ 350 };
		std::atomic_bool      g_hotkeyWave{ true };

		std::atomic_bool g_dirty{ false };
		std::chrono::steady_clock::time_point g_lastChange{};

		std::mutex g_cvMu;
		std::condition_variable g_cv;

		bool g_started = false;
		bool g_stop = false;
		std::thread g_worker;

		std::string g_profileId = "default";
		fs::path g_settingsPath;

		static float ClampPct(float v) { return std::clamp(v, 2.0f, 95.0f); }
		static float ClampScale(float v) { return std::clamp(v, 0.0f, 1.0f); }
		static int   ClampBleed(int v) { return std::clamp(v, 10, 30); }
		static float ClampRadius(float v) { return std::clamp(v, 128.0f, 20000.0f); }
		static int   ClampCooldown(int v) { return std::clamp(v, 0, 5000); }

		static fs::path GetRuntimeDir()
		{
			wchar_t buf[MAX_PATH]{};
			GetModuleFileNameW(nullptr, buf, MAX_PATH);
			return fs::path(buf).parent_path();
		}

		static fs::path GetProfileIdIniPath()
		{
			return GetRuntimeDir() / "Data" / "SKSE" / "Plugins" / "TFDEngine.profile.ini";
		}

		static fs::path GetSkyrimDocumentsDir()
		{
			PWSTR docsPath = nullptr;
			const HRESULT hr = SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docsPath);
			if (FAILED(hr) || !docsPath) {
				return GetRuntimeDir();
			}

			fs::path p(docsPath);
			CoTaskMemFree(docsPath);
			return p / "My Games" / "Skyrim Special Edition";
		}

		static fs::path GetSettingsPathForProfile(const std::string& profileId)
		{
			return GetSkyrimDocumentsDir() / "TFDEngine" / "profiles" / (profileId + ".ini");
		}

		static std::string SanitizeProfileId(std::string id)
		{
			if (id.empty()) {
				return "default";
			}
			for (auto& c : id) {
				const unsigned char uc = static_cast<unsigned char>(c);
				if (!(std::isalnum(uc) || c == '_' || c == '-')) {
					c = '_';
				}
			}
			return id;
		}

		static std::unordered_map<std::string, std::string> ReadKeyValueFile(const fs::path& path)
		{
			std::unordered_map<std::string, std::string> out;
			std::ifstream f(path);
			if (!f.is_open()) {
				return out;
			}

			std::string line;
			while (std::getline(f, line)) {
				if (line.empty()) continue;
				if (line[0] == ';' || line[0] == '#') continue;

				auto eq = line.find('=');
				if (eq == std::string::npos) continue;

				auto k = line.substr(0, eq);
				auto v = line.substr(eq + 1);

				while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
				while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());

				out.emplace(std::move(k), std::move(v));
			}
			return out;
		}

		static void AtomicWriteText(const fs::path& path, const std::string& content)
		{
			fs::create_directories(path.parent_path());

			fs::path tmp = path;
			tmp += L".tmp";

			{
				std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
				if (!f.is_open()) {
					spdlog::warn("[TFD][Settings] cannot write temp: {}", tmp.string());
					return;
				}
				f.write(content.data(), static_cast<std::streamsize>(content.size()));
				f.flush();
			}

			const auto wTmp = tmp.wstring();
			const auto wDst = path.wstring();
			if (!MoveFileExW(wTmp.c_str(), wDst.c_str(), MOVEFILE_REPLACE_EXISTING)) {
				const DWORD err = GetLastError();
				spdlog::warn("[TFD][Settings] MoveFileExW failed err={}", static_cast<unsigned long>(err));
			}
		}

		static std::string LoadProfileId()
		{
			const auto path = GetProfileIdIniPath();
			auto kv = ReadKeyValueFile(path);

			auto it = kv.find("profile_id");
			if (it == kv.end() || it->second.empty()) {
				spdlog::warn("[TFD][Settings] profile_id missing in {} (using 'default')", path.string());
				return "default";
			}
			return SanitizeProfileId(it->second);
		}

		static void MarkDirty()
		{
			g_dirty.store(true);
			g_lastChange = std::chrono::steady_clock::now();
			g_cv.notify_all();
		}

		static std::string SerializeIni()
		{
			std::string s;
			s += "enabled=" + std::to_string(GetEnabled() ? 1 : 0) + "\n";
			s += "defeatThresholdPct=" + std::to_string(GetDefeatThresholdPct()) + "\n";
			s += "playerGuardThresholdPct=" + std::to_string(GetPlayerGuardThresholdPct()) + "\n";
			s += "playerGuardDamageScale=" + std::to_string(GetPlayerGuardDamageScale()) + "\n";
			s += "allyDownedThresholdPct=" + std::to_string(GetAllyDownedThresholdPct()) + "\n";
			s += "enemyDownedThresholdPct=" + std::to_string(GetEnemyDownedThresholdPct()) + "\n";
			s += "bleedWindowSeconds=" + std::to_string(GetBleedWindowSeconds()) + "\n";
			s += "scanRadius=" + std::to_string(GetScanRadius()) + "\n";
			s += "sweepRadius=" + std::to_string(GetSweepRadius()) + "\n";
			s += "hotkeyEnabled=" + std::to_string(GetHotkeyEnabled() ? 1 : 0) + "\n";
			s += "hotkeyScanCode=" + std::to_string(GetHotkeyScanCode()) + "\n";
			s += "hotkeyCooldownMs=" + std::to_string(GetHotkeyCooldownMs()) + "\n";
			s += "hotkeyWave=" + std::to_string(GetHotkeyWave() ? 1 : 0) + "\n";
			return s;
		}

		static void SaveNow()
		{
			if (g_settingsPath.empty()) return;
			AtomicWriteText(g_settingsPath, SerializeIni());
			spdlog::info("[TFD][Settings] saved -> {}", g_settingsPath.string());
		}

		static void LoadFromDisk()
		{
			auto kv = ReadKeyValueFile(g_settingsPath);
			if (kv.empty()) {
				spdlog::info("[TFD][Settings] no settings yet (defaults). path={}", g_settingsPath.string());
				return;
			}

			auto getBool = [&](const char* k, bool def) {
				auto it = kv.find(k);
				if (it == kv.end()) return def;
				return (it->second == "1" || it->second == "true" || it->second == "TRUE");
				};

			auto getInt = [&](const char* k, int def) {
				auto it = kv.find(k);
				if (it == kv.end()) return def;
				try { return std::stoi(it->second); }
				catch (...) { return def; }
				};

			auto getUInt = [&](const char* k, std::uint32_t def) {
				auto it = kv.find(k);
				if (it == kv.end()) return def;
				try { return static_cast<std::uint32_t>(std::stoul(it->second)); }
				catch (...) { return def; }
				};

			auto getFloat = [&](const char* k, float def) {
				auto it = kv.find(k);
				if (it == kv.end()) return def;
				try { return std::stof(it->second); }
				catch (...) { return def; }
				};

			g_enabled.store(getBool("enabled", g_enabled.load()));
			g_defeatThresholdPct.store(ClampPct(getFloat("defeatThresholdPct", g_defeatThresholdPct.load())));
			g_playerGuardThresholdPct.store(ClampPct(getFloat("playerGuardThresholdPct", g_playerGuardThresholdPct.load())));
			g_playerGuardDamageScale.store(ClampScale(getFloat("playerGuardDamageScale", g_playerGuardDamageScale.load())));
			g_allyDownedThresholdPct.store(ClampPct(getFloat("allyDownedThresholdPct", g_allyDownedThresholdPct.load())));
			g_enemyDownedThresholdPct.store(ClampPct(getFloat("enemyDownedThresholdPct", g_enemyDownedThresholdPct.load())));
			g_bleedWindowSeconds.store(ClampBleed(getInt("bleedWindowSeconds", g_bleedWindowSeconds.load())));
			g_scanRadius.store(ClampRadius(getFloat("scanRadius", g_scanRadius.load())));
			g_sweepRadius.store(ClampRadius(getFloat("sweepRadius", g_sweepRadius.load())));
			g_hotkeyEnabled.store(getBool("hotkeyEnabled", g_hotkeyEnabled.load()));
			g_hotkeyScanCode.store(getUInt("hotkeyScanCode", g_hotkeyScanCode.load()));
			g_hotkeyCooldownMs.store(ClampCooldown(getInt("hotkeyCooldownMs", g_hotkeyCooldownMs.load())));
			g_hotkeyWave.store(getBool("hotkeyWave", g_hotkeyWave.load()));

			spdlog::info("[TFD][Settings] loaded profile='{}' from {}", g_profileId, g_settingsPath.string());
		}

		static void WorkerLoop()
		{
			std::unique_lock lk(g_cvMu);

			while (!g_stop) {
				g_cv.wait(lk, [] { return g_stop || g_dirty.load(); });
				if (g_stop) break;

				while (!g_stop) {
					const auto now = std::chrono::steady_clock::now();
					const auto elapsed = now - g_lastChange;
					const auto need = std::chrono::milliseconds(700);

					if (elapsed >= need) break;
					g_cv.wait_for(lk, need - elapsed);
				}
				if (g_stop) break;

				if (g_dirty.exchange(false)) {
					lk.unlock();
					SaveNow();
					lk.lock();
				}
			}
		}

		static void EnsureWorkerStarted()
		{
			std::scoped_lock lk(g_cvMu);
			if (g_started) return;

			g_stop = false;
			g_worker = std::thread(WorkerLoop);
			g_worker.detach();
			g_started = true;

			spdlog::info("[TFD][Settings] autosave worker started");
		}
	}

	void InitProfileIniPersistence()
	{
		g_profileId = LoadProfileId();
		g_settingsPath = GetSettingsPathForProfile(g_profileId);

		spdlog::info("[TFD][Settings] active profile_id='{}'", g_profileId);
		spdlog::info("[TFD][Settings] settings path={}", g_settingsPath.string());

		LoadFromDisk();
		EnsureWorkerStarted();
	}

	void FlushNow()
	{
		SaveNow();
	}

	bool GetEnabled() { return g_enabled.load(); }
	void SetEnabled(bool a_enabled)
	{
		g_enabled.store(a_enabled);
		MarkDirty();
	}

	float GetDefeatThresholdPct() { return g_defeatThresholdPct.load(); }
	void SetDefeatThresholdPct(float a_pct)
	{
		g_defeatThresholdPct.store(ClampPct(a_pct));
		if (g_playerGuardThresholdPct.load() < g_defeatThresholdPct.load()) {
			g_playerGuardThresholdPct.store(g_defeatThresholdPct.load());
		}
		MarkDirty();
	}

	float GetPlayerGuardThresholdPct() { return (std::max)(g_playerGuardThresholdPct.load(), g_defeatThresholdPct.load()); }
	void SetPlayerGuardThresholdPct(float a_pct)
	{
		g_playerGuardThresholdPct.store((std::max)(ClampPct(a_pct), g_defeatThresholdPct.load()));
		MarkDirty();
	}

	float GetPlayerGuardDamageScale() { return ClampScale(g_playerGuardDamageScale.load()); }
	void SetPlayerGuardDamageScale(float a_scale)
	{
		g_playerGuardDamageScale.store(ClampScale(a_scale));
		MarkDirty();
	}

	float GetAllyDownedThresholdPct() { return g_allyDownedThresholdPct.load(); }
	void SetAllyDownedThresholdPct(float a_pct)
	{
		g_allyDownedThresholdPct.store(ClampPct(a_pct));
		MarkDirty();
	}

	float GetEnemyDownedThresholdPct() { return g_enemyDownedThresholdPct.load(); }
	void SetEnemyDownedThresholdPct(float a_pct)
	{
		g_enemyDownedThresholdPct.store(ClampPct(a_pct));
		MarkDirty();
	}

	int GetBleedWindowSeconds() { return g_bleedWindowSeconds.load(); }
	void SetBleedWindowSeconds(int a_seconds)
	{
		g_bleedWindowSeconds.store(ClampBleed(a_seconds));
		MarkDirty();
	}

	float GetScanRadius() { return g_scanRadius.load(); }
	void SetScanRadius(float a_radius)
	{
		g_scanRadius.store(ClampRadius(a_radius));
		MarkDirty();
	}

	float GetSweepRadius() { return g_sweepRadius.load(); }
	void SetSweepRadius(float a_radius)
	{
		g_sweepRadius.store(ClampRadius(a_radius));
		MarkDirty();
	}

	bool GetHotkeyEnabled() { return g_hotkeyEnabled.load(); }
	void SetHotkeyEnabled(bool a_enabled)
	{
		g_hotkeyEnabled.store(a_enabled);
		MarkDirty();
	}

	std::uint32_t GetHotkeyScanCode() { return g_hotkeyScanCode.load(); }
	void SetHotkeyScanCode(std::uint32_t a_code)
	{
		g_hotkeyScanCode.store(a_code);
		MarkDirty();
	}

	int GetHotkeyCooldownMs() { return g_hotkeyCooldownMs.load(); }
	void SetHotkeyCooldownMs(int a_ms)
	{
		g_hotkeyCooldownMs.store(ClampCooldown(a_ms));
		MarkDirty();
	}

	bool GetHotkeyWave() { return g_hotkeyWave.load(); }
	void SetHotkeyWave(bool a_enabled)
	{
		g_hotkeyWave.store(a_enabled);
		MarkDirty();
	}
}