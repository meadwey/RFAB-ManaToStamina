#include "ManaToStamina.h"
#include "DebugLog.h"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "RE/P/PlayerCharacter.h"
#include "RE/S/SendHUDMessage.h"
#include "RE/T/TESGlobal.h"
#include "RE/T/TESForm.h"
#include "RE/I/IFormFactory.h"
#include "RE/S/SpellItem.h"
#include "RE/M/MagicItem.h"
#include "RE/M/MagicSystem.h"
#include "RE/E/Effect.h"
#include "RE/E/EffectSetting.h"
#include "RE/M/MagicTarget.h"
#include "RE/A/ActiveEffect.h"
#include "RE/A/ActorValues.h"
#include "SKSE/API.h"
#include "SKSE/Interfaces.h"
#include "SKSE/Logger.h"

namespace ManaToStamina
{
	namespace
	{
		constexpr auto kPollInterval{ std::chrono::seconds(1) };
		constexpr std::uint32_t kRecordType{ 'MTS1' };
		constexpr std::uint32_t kRecordVersion{ 1 };

		RE::TESGlobal* _manaDrainUpkeep{ nullptr };
		constexpr auto kManaDrainUpkeepGlobal{ "RFAB_Global_ManaDrain_Upkeep" };

		bool _enabled{ false };
		bool _fallbackToMagicka{ true };
		bool _showHUD{ true };
		std::string _toggleAbilityEditorID{ "RFABManaToStamina_Toggle" };

		RE::SpellItem* _toggleSpell{ nullptr };

		HMODULE _module{ nullptr };

		std::mutex              _mutex;
		std::condition_variable _wakeup;
		std::thread             _timer;
		std::atomic_bool        _running{ false };
		std::atomic_bool        _queued{ false };

		void OnGameTick()
		{
			_queued.store(false, std::memory_order_release);
			if (!_enabled) {
				return;
			}

			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				return;
			}

			if (!_manaDrainUpkeep) {
				_manaDrainUpkeep = RE::TESForm::LookupByEditorID<RE::TESGlobal>(kManaDrainUpkeepGlobal);
				if (!_manaDrainUpkeep) {
					SKSE::log::warn("RFAB_Global_ManaDrain_Upkeep not found, redirect disabled");
					return;
				}
			}

			const float upkeep = _manaDrainUpkeep->value;
			if (upkeep <= 0.0F) {
				return;
			}

			const float magicka = player->GetActorValue(RE::ActorValue::kMagicka);
			const float stamina = player->GetActorValue(RE::ActorValue::kStamina);

			if (stamina >= upkeep) {
				player->ModActorValue(RE::ActorValue::kStamina, -upkeep);
				player->ModActorValue(RE::ActorValue::kMagicka, upkeep);
				return;
			} else if (_fallbackToMagicka) {
				// Let RFAB's natural drain proceed - we don't compensate
				return;
			} else {
				player->ModActorValue(RE::ActorValue::kStamina, -stamina);
				// don't drain magicka if fallback disabled; RFAB may behave as normal
				return;
			}
		}

		void TimerLoop()
		{
			std::unique_lock lock(_mutex);
			while (_running.load(std::memory_order_acquire)) {
				if (_wakeup.wait_for(lock, kPollInterval, []() {
						return !_running.load(std::memory_order_acquire);
					})) {
					break;
				}
				if (!_running.load(std::memory_order_acquire)) {
					break;
				}
				if (!_queued.exchange(true, std::memory_order_acq_rel)) {
					const auto* tasks = SKSE::GetTaskInterface();
					if (tasks) {
						tasks->AddTask(OnGameTick);
					} else {
						_queued.store(false, std::memory_order_release);
					}
				}
			}
		}

		RE::SpellItem* CreateToggleSpell(const std::string& a_editorID)
		{
			auto* spell = RE::IFormFactory::Create<RE::SpellItem>();
			if (!spell) {
				SKSE::log::error("failed to create toggle spell");
				return nullptr;
			}
			spell->SetFormEditorID(a_editorID.c_str());
			spell->data.costOverride = 0;
			spell->data.flags = RE::SpellItem::SpellFlag::kNone;
			spell->data.spellType = RE::MagicSystem::SpellType::kPower;
			spell->data.castingType = RE::MagicSystem::CastingType::kFireAndForget;
			spell->data.delivery = RE::MagicSystem::Delivery::kSelf;
			spell->data.chargeTime = 0.0F;
			spell->data.castDuration = 0.0F;
			spell->data.range = 0.0F;
			spell->data.castingPerk = nullptr;
			spell->hostileCount = 0;
			spell->avEffectSetting = nullptr;
			spell->preloadCount = 0;

			auto* effect = new RE::Effect();
			// Add a dummy effect - simplest approach
			spell->effects.push_back(effect);
			spell->InitItemImpl();
			return spell;
		}
	}

	Watcher& Watcher::GetSingleton()
	{
		static Watcher singleton;
		return singleton;
	}

	void Watcher::OnAttach(HMODULE a_module)
	{
		_module = a_module;
	}

	void Watcher::SetupSerialization()
	{
		const auto* intfc = SKSE::GetSerializationInterface();
		if (!intfc) {
			return;
		}
		intfc->SetUniqueID(kRecordType);
	}

	void Watcher::Toggle()
	{
		_enabled = !_enabled;
		if (_showHUD) {
			const std::string msg = "ManaToStamina: " + std::string(_enabled ? "ON" : "OFF");
			RE::SendHUDMessage::ShowHUDMessage(msg.c_str());
		}
		SKSE::log::info("toggled: {}", _enabled ? "ON" : "OFF");
	}

	void Watcher::Install()
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			SKSE::log::error("no player singleton");
			return;
		}

		// Try to load config
		wchar_t buffer[MAX_PATH]{};
		if (_module && GetModuleFileNameW(_module, buffer, MAX_PATH)) {
			namespace fs = std::filesystem;
			const fs::path iniPath = fs::path{ buffer }.parent_path() / "RFABManaToStamina.ini";
			std::ifstream file{ iniPath };
			if (file) {
				std::string line;
				while (std::getline(file, line)) {
					// simple parse
					const auto eq = line.find('=');
					if (eq == std::string::npos) continue;
					std::string key = line.substr(0, eq);
					std::string val = line.substr(eq + 1);
					// trim
					if (key.find("ToggleAbilityEditorID") != std::string::npos) _toggleAbilityEditorID = val;
					if (key.find("EnabledByDefault") != std::string::npos) _enabled = (val.find("true") != std::string::npos);
					if (key.find("ShowHUDMessages") != std::string::npos) _showHUD = (val.find("true") != std::string::npos);
					if (key.find("FallbackToMagicka") != std::string::npos) _fallbackToMagicka = (val.find("true") != std::string::npos);
				}
			}
		}

		_manaDrainUpkeep = RE::TESForm::LookupByEditorID<RE::TESGlobal>(kManaDrainUpkeepGlobal);

		_toggleSpell = CreateToggleSpell(_toggleAbilityEditorID);
		if (_toggleSpell && player) {
			// Add power spell to player
			player->AddSpell(_toggleSpell);
			SKSE::log::info("added toggle power spell: {}", _toggleAbilityEditorID);
		}

		if (_running.load(std::memory_order_acquire)) {
			return;
		}
		_running.store(true, std::memory_order_release);
		_timer = std::thread{ TimerLoop };
		_timer.detach();

		SKSE::log::info("installed, enabled={}", _enabled);
	}

	void Watcher::Unload()
	{
		_running.store(false, std::memory_order_release);
		_wakeup.notify_all();
	}
}
