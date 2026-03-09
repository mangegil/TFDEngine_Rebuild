#pragma once

#include "RE/Skyrim.h"

#include <string>
#include <unordered_map>
#include <mutex>

namespace TFD::Util
{
	class EditorIdCache
	{
	public:
		static EditorIdCache& Get()
		{
			static EditorIdCache inst;
			return inst;
		}

		std::string GetEditorId(RE::TESForm* form)
		{
			if (!form) {
				return {};
			}

			BuildOnce();

			std::scoped_lock lock(mutex);
			const auto id = form->GetFormID();
			const auto it = byFormId.find(id);
			if (it != byFormId.end()) {
				return it->second;
			}
			return {};
		}

		void Reset()
		{
			std::scoped_lock lock(mutex);
			byFormId.clear();
			built = false;
		}

	private:
		struct ReadLock
		{
			RE::BSReadWriteLock& lock;
			explicit ReadLock(RE::BSReadWriteLock& aLock) : lock(aLock) { lock.LockForRead(); }
			~ReadLock() { lock.UnlockForRead(); }
		};

		void BuildOnce()
		{
			if (built) {
				return;
			}

			std::scoped_lock lock(mutex);
			if (built) {
				return;
			}

			auto pair = RE::TESForm::GetAllFormsByEditorID();
			auto* mapPtr = pair.first;
			auto& rwLock = pair.second.get();

			if (!mapPtr) {
				built = true;
				return;
			}

			ReadLock r(rwLock);

			for (auto& entry : *mapPtr) {
				auto* f = entry.second;
				if (!f) {
					continue;
				}

				const char* eid = entry.first.c_str();
				if (!eid || !eid[0]) {
					continue;
				}

				byFormId.emplace(f->GetFormID(), std::string(eid));
			}

			built = true;
		}

		std::unordered_map<RE::FormID, std::string> byFormId;
		std::mutex mutex;
		bool built{ false };
	};

	inline std::string GetEditorId(RE::TESForm* form)
	{
		return EditorIdCache::Get().GetEditorId(form);
	}
}
