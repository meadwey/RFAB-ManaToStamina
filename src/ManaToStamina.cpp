#include "ManaToStamina.h"
#include "DebugLog.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "RE/A/Actor.h"
#include "RE/A/ActorValues.h"
#include "RE/B/BSTEvent.h"
#include "RE/E/Effect.h"
#include "RE/E/EffectSetting.h"
#include "RE/I/IFormFactory.h"
#include "RE/M/MagicItem.h"
#include "RE/M/MagicSystem.h"
#include "RE/P/PlayerCharacter.h"
#include "RE/S/ScriptEventSourceHolder.h"
#include "RE/S/SendHUDMessage.h"
#include "RE/S/SpellItem.h"
#include "RE/T/TESForm.h"
#include "RE/T/TESGlobal.h"
#include "RE/T/TESSpellCastEvent.h"
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

		// Effective settings (RFABManaToStamina.ini).
		std::string _togglePowerEditorID{ Config::kTogglePowerEditorIDDefault };
		std::string _manaDrainUpkeepGlobal{ Config::kManaDrainUpkeepGlobalDefault };
		std::string _placeholderEffect{ Config::kPlaceholderEffectDefault };
		bool        _enabledByDefault{ false };
		bool        _showHUD{ true };

		// Touched on the game thread only.
		bool           _enabled{ false };
		bool           _enabledFromSave{ false };
		RE::TESGlobal* _upkeep{ nullptr };
		RE::SpellItem* _toggleSpell{ nullptr };

		HMODULE _module{ nullptr };

		// Touched by the timer thread.
		std::mutex              _mutex;
		std::condition_variable _wakeup;
		std::thread             _timer;
		std::atomic_bool        _running{ false };
		std::atomic_bool        _queued{ false };

		std::string_view Trim(std::string_view a_line)
		{
			const auto first = a_line.find_first_not_of(" \t\r\n");
			if (first == std::string_view::npos) {
				return {};
			}
			const auto last = a_line.find_last_not_of(" \t\r\n");
			return a_line.substr(first, last - first + 1);
		}

		bool ParseBool(std::string_view a_value, bool a_default)
		{
			std::string lowered(a_value);
			for (auto& c : lowered) {
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}
			if (lowered == "1" || lowered == "true" || lowered == "yes" || lowered == "on") {
				return true;
			}
			if (lowered == "0" || lowered == "false" || lowered == "no" || lowered == "off") {
				return false;
			}
			return a_default;
		}

		// Reads RFABManaToStamina.ini from the DLL's own folder. Simple
		// "key=value" format: sections are skipped, ';' and '#' start a
		// comment, unknown keys are logged instead of being silently ignored.
		void LoadConfig()
		{
			namespace fs = std::filesystem;

			std::string togglePower = Config::kTogglePowerEditorIDDefault;
			std::string upkeepGlobal = Config::kManaDrainUpkeepGlobalDefault;
			std::string placeholder = Config::kPlaceholderEffectDefault;
			bool        enabledByDefault = false;
			bool        showHUD = true;

			wchar_t buffer[MAX_PATH]{};
			if (!_module || !GetModuleFileNameW(_module, buffer, MAX_PATH)) {
				SKSE::log::error("cannot resolve the plugin path, RFABManaToStamina.ini is not read");
				return;
			}

			const fs::path iniPath = fs::path{ buffer }.parent_path() / "RFABManaToStamina.ini";
			std::ifstream file{ iniPath };
			if (!file) {
				SKSE::log::info("no RFABManaToStamina.ini, defaults in use");
			} else {
				std::string line;
				while (std::getline(file, line)) {
					const std::string_view view = Trim(line);
					if (view.empty() || view.front() == ';' || view.front() == '#' || view.front() == '[') {
						continue;
					}

					const auto eq = view.find('=');
					if (eq == std::string_view::npos) {
						SKSE::log::warn("RFABManaToStamina.ini: ignoring line '{}'", view);
						continue;
					}

					const auto key = Trim(view.substr(0, eq));
					const auto value = Trim(view.substr(eq + 1));

					if (key == "ToggleAbilityEditorID") {
						togglePower = std::string{ value };
					} else if (key == "ManaDrainUpkeepGlobal") {
						upkeepGlobal = std::string{ value };
					} else if (key == "PlaceholderEffect") {
						placeholder = std::string{ value };
					} else if (key == "EnabledByDefault") {
						enabledByDefault = ParseBool(value, enabledByDefault);
					} else if (key == "ShowHUDMessages") {
						showHUD = ParseBool(value, showHUD);
					} else {
						SKSE::log::warn("RFABManaToStamina.ini: unknown key '{}'", key);
					}
				}
			}

			_togglePowerEditorID = togglePower.empty() ? Config::kTogglePowerEditorIDDefault : togglePower;
			_manaDrainUpkeepGlobal = upkeepGlobal.empty() ? Config::kManaDrainUpkeepGlobalDefault : upkeepGlobal;
			_placeholderEffect = placeholder.empty() ? Config::kPlaceholderEffectDefault : placeholder;
			_enabledByDefault = enabledByDefault;
			_showHUD = showHUD;

			SKSE::log::info(
				"config: ToggleAbilityEditorID={}, ManaDrainUpkeepGlobal={}, PlaceholderEffect={}, EnabledByDefault={}, ShowHUDMessages={}",
				_togglePowerEditorID,
				_manaDrainUpkeepGlobal,
				_placeholderEffect,
				_enabledByDefault,
				_showHUD);
		}

		// Creates the runtime lesser power the player uses to flip the
		// redirect. It carries a single magnitude-0 placeholder effect so the
		// spell is valid and castable; casting it only raises a spell-cast
		// event, which Toggle() reacts to.
		RE::SpellItem* CreateTogglePower()
		{
			auto* spell = RE::IFormFactory::Create<RE::SpellItem>();
			if (!spell) {
				SKSE::log::error("failed to create the '{}' toggle power", _togglePowerEditorID);
				return nullptr;
			}

			spell->SetFormEditorID(_togglePowerEditorID.c_str());
			spell->SetFullName("Перенаправление магического истощения на выносливость");
			spell->data.costOverride = 0;
			spell->data.flags = RE::SpellItem::SpellFlag::kNone;
			spell->data.spellType = RE::MagicSystem::SpellType::kLesserPower;
			spell->data.castingType = RE::MagicSystem::CastingType::kFireAndForget;
			spell->data.delivery = RE::MagicSystem::Delivery::kSelf;
			spell->data.chargeTime = 0.0F;
			spell->data.castDuration = 0.0F;
			spell->data.range = 0.0F;
			spell->data.castingPerk = nullptr;
			spell->hostileCount = 0;
			spell->avEffectSetting = nullptr;
			spell->preloadCount = 0;

			auto* placeholder = RE::TESForm::LookupByEditorID<RE::EffectSetting>(_placeholderEffect);
			if (placeholder) {
				auto* effect = new RE::Effect();
				effect->baseEffect = placeholder;
				effect->effectItem.magnitude = 0.0F;
				effect->effectItem.area = 0;
				effect->effectItem.duration = 0;
				spell->effects.push_back(effect);
			} else {
				SKSE::log::warn("placeholder effect '{}' not found, the toggle power has no effects", _placeholderEffect);
			}

			spell->InitItemImpl();

			SKSE::log::info("created '{}' toggle power (form {:08X})", _togglePowerEditorID, spell->formID);
			return spell;
		}

		// One check per second, on the game thread. RFAB drains magicka by
		// RFAB_Global_ManaDrain_Upkeep every second while its auras are up.
		// We cannot edit RFAB, so to make the upkeep cost eat *only* stamina
		// the plugin (1) refunds exactly that much magicka, leaving magicka
		// untouched, and (2) takes the same amount out of stamina. Net effect
		// on magicka is zero, on stamina it is the full upkeep.
		void Poll()
		{
			if (!_enabled) {
				return;
			}

			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				return;
			}

			if (!_upkeep) {
				_upkeep = RE::TESForm::LookupByEditorID<RE::TESGlobal>(_manaDrainUpkeepGlobal);
				if (!_upkeep) {
					SKSE::log::warn("'{}' not found (is RFAB.esp loaded?), the redirect is idle", _manaDrainUpkeepGlobal);
					return;
				}
			}

			const float upkeep = _upkeep->value;
			if (!(upkeep > 0.0F)) {
				return;
			}

			const float stamina = player->GetActorValue(RE::ActorValue::kStamina);
			const float staminaMax = player->GetActorValueMax(RE::ActorValue::kStamina);
			const bool  exhausted = staminaMax > 0.0F && (stamina / staminaMax) <= 0.10F;

			// RFAB boosts its supported spells' useful effects while
			// GetActorValuePercent(Magicka) >= 0.1 and stops boosting them
			// below that. The mod cannot edit RFAB, so stamina is mapped onto
			// that same gate: while stamina is above 10% the mana RFAB took is
			// refunded (mana is never spent), and once stamina falls to 10% or
			// less magicka is held below 10%, which robs the auras of their
			// useful properties while the drain keeps running.
			if (exhausted) {
				const float magicka = player->GetActorValue(RE::ActorValue::kMagicka);
				if (magicka > 0.0F) {
					player->DamageActorValue(RE::ActorValue::kMagicka, magicka);
				}
			} else {
				player->RestoreActorValue(RE::ActorValue::kMagicka, upkeep);
			}

			// The upkeep is always paid from stamina; it floors at zero, so
			// the auras keep running indefinitely.
			player->DamageActorValue(RE::ActorValue::kStamina, upkeep);
		}

		void OnGameTick()
		{
			_queued.store(false, std::memory_order_release);
			Poll();
		}

		// Owns nothing but the clock. Every game access happens in OnGameTick.
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

		// Reacts to the player casting the toggle power.
		class SpellCastSink : public RE::BSTEventSink<RE::TESSpellCastEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESSpellCastEvent* a_event, RE::BSTEventSource<RE::TESSpellCastEvent>*) override
			{
				if (a_event && _toggleSpell) {
					auto* player = RE::PlayerCharacter::GetSingleton();
					if (player && a_event->object.get() == player && a_event->spell == _toggleSpell->formID) {
						Watcher::GetSingleton().Toggle();
					}
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		SpellCastSink _spellCastSink;

		void RegisterSpellCastSink()
		{
			auto* holder = RE::ScriptEventSourceHolder::GetSingleton();
			if (!holder) {
				SKSE::log::error("no ScriptEventSourceHolder, the toggle power cannot be detected");
				return;
			}
			holder->AddEventSink<RE::TESSpellCastEvent>(&_spellCastSink);
			SKSE::log::info("spell-cast event sink registered");
		}

		void OnSave(SKSE::SerializationInterface* a_intfc)
		{
			const std::uint32_t enabled = _enabled ? 1U : 0U;
			if (!a_intfc->WriteRecord(kRecordType, kRecordVersion, enabled)) {
				SKSE::log::error("failed to write the cosave record");
			}
		}

		void OnLoad(SKSE::SerializationInterface* a_intfc)
		{
			std::uint32_t type = 0;
			std::uint32_t version = 0;
			std::uint32_t length = 0;
			while (a_intfc->GetNextRecordInfo(type, version, length)) {
				if (type != kRecordType) {
					continue;
				}
				if (version != kRecordVersion || length != sizeof(std::uint32_t)) {
					SKSE::log::warn("ignoring a cosave record (version {}, length {})", version, length);
					continue;
				}
				std::uint32_t enabled = 0;
				if (a_intfc->ReadRecordData(enabled) == sizeof(enabled)) {
					_enabled = enabled != 0;
					_enabledFromSave = true;
					SKSE::log::info("restored redirect state from the cosave: {}", _enabled ? "ON" : "OFF");
				}
			}
		}

		void OnRevert(SKSE::SerializationInterface*)
		{
			_enabledFromSave = false;
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
			SKSE::log::error("failed to get the SKSE serialization interface");
			return;
		}

		intfc->SetUniqueID(kRecordType);
		intfc->SetSaveCallback(OnSave);
		intfc->SetLoadCallback(OnLoad);
		intfc->SetRevertCallback(OnRevert);

		SKSE::log::info("cosave callbacks registered (unique id {:08X})", kRecordType);
	}

	void Watcher::Toggle()
	{
		_enabled = !_enabled;
		_enabledFromSave = true;

		if (_showHUD) {
			const std::string msg = _enabled ?
			                            "ManaToStamina: ON (spells drain stamina)" :
			                            "ManaToStamina: OFF (spells drain magicka)";
			RE::SendHUDMessage::ShowHUDMessage(msg.c_str());
		}

		SKSE::log::info("redirect toggled: {}", _enabled ? "ON" : "OFF");
	}

	void Watcher::Install()
	{
		LoadConfig();

		if (!_enabledFromSave) {
			_enabled = _enabledByDefault;
		}

		if (!_toggleSpell) {
			_toggleSpell = CreateTogglePower();
		}

		auto* player = RE::PlayerCharacter::GetSingleton();
		if (player && _toggleSpell) {
			player->AddSpell(_toggleSpell);
			SKSE::log::info("added the '{}' toggle power to the player", _togglePowerEditorID);
		}

		if (!_running.load(std::memory_order_acquire)) {
			RegisterSpellCastSink();

			_running.store(true, std::memory_order_release);
			_timer = std::thread{ TimerLoop };
			// Detached on purpose: joining under the loader lock in DllMain is
			// not safe, and SKSE never unloads a plugin while the game keeps
			// running, so the only thing that has to happen at exit is the
			// running flag being cleared.
			_timer.detach();
		}

		SKSE::log::info("installed, redirect {}", _enabled ? "ON" : "OFF");
	}

	void Watcher::Unload()
	{
		_running.store(false, std::memory_order_release);
		_wakeup.notify_all();
	}
}
