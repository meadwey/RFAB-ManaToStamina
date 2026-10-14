#include "DebugLog.h"
#include "ManaToStamina.h"

#include <Windows.h>

#include "SKSE/API.h"
#include "SKSE/Interfaces.h"
#include "SKSE/Logger.h"
#include "SKSE/Version.h"

namespace
{
	constexpr REL::Version kPluginVersion{ 0, 1, 0, 0 };

	void OnMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		switch (a_msg->type) {
		case SKSE::MessagingInterface::kPostLoadGame:
		case SKSE::MessagingInterface::kNewGame:
			// kPostLoad fires before the save is read, so
			// RE::PlayerCharacter::GetSingleton() is still null there and can
			// only be relied on once a game is actually loaded.
			SKSE::log::info("game loaded -> starting the mana-to-stamina watcher");
			ManaToStamina::Watcher::GetSingleton().Install();
			break;
		default:
			break;
		}
	}
}

// 1.5.97's MessagingInterface has no shutdown message, so the timer thread has
// to be stopped from the module unload instead: a joinable std::thread whose
// destructor runs after the DLL is unmapped would execute freed code.
BOOL APIENTRY DllMain(HMODULE a_module, DWORD a_reason, LPVOID)
{
	if (a_reason == DLL_PROCESS_ATTACH) {
		// Needed to find RFABManaToStamina.ini next to the DLL.
		ManaToStamina::Watcher::GetSingleton().OnAttach(a_module);
	} else if (a_reason == DLL_PROCESS_DETACH) {
		ManaToStamina::Watcher::GetSingleton().Unload();
	}

	return TRUE;
}

extern "C" __declspec(dllexport)
bool SKSEPlugin_Query(const SKSE::QueryInterface* a_skse, SKSE::PluginInfo* a_info)
{
	a_info->infoVersion = SKSE::PluginInfo::kVersion;
	a_info->name = "RFAB-ManaToStamina";
	a_info->version = kPluginVersion.pack();

	if (a_skse->IsEditor()) {
		return false;
	}
	if (a_skse->RuntimeVersion() != SKSE::RUNTIME_SSE_1_5_97) {
		return false;
	}
	return true;
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);

	DebugLog::InitLogging("RFABManaToStamina");

	if (a_skse->RuntimeVersion() != SKSE::RUNTIME_SSE_1_5_97) {
		SKSE::log::error("unsupported runtime version, Skyrim SE 1.5.97 is required");
		return false;
	}

	SKSE::log::info(
		"RFAB-ManaToStamina v{}.{}.{} loaded, runtime {}.{}.{}",
		kPluginVersion.major(),
		kPluginVersion.minor(),
		kPluginVersion.patch(),
		a_skse->RuntimeVersion().major(),
		a_skse->RuntimeVersion().minor(),
		a_skse->RuntimeVersion().patch());

	// Has to happen before the first save is read; Install only runs on
	// kPostLoadGame, which is too late for that.
	ManaToStamina::Watcher::GetSingleton().SetupSerialization();

	const auto* messaging = SKSE::GetMessagingInterface();
	if (!messaging) {
		SKSE::log::error("failed to get the SKSE messaging interface");
		return false;
	}

	// 1.5.97 only has the sender-less overload: the "const char* a_sender" one
	// is an Anniversary Edition addition and always fails on this runtime.
	if (!messaging->RegisterListener(OnMessage)) {
		SKSE::log::error("failed to register the SKSE messaging listener");
		return false;
	}

	return true;
}
