#include "ManaToStamina.h"
#include "DebugLog.h"

#include <Windows.h>

#include "SKSE/API.h"
#include "SKSE/Interfaces.h"
#include "SKSE/Logger.h"
#include "SKSE/Trampoline.h"

using namespace SKSE::log;
using namespace SKSE::stl;

static void SKSEMessageHandler(SKSE::MessagingInterface::Message* a_msg)
{
	switch (a_msg->type) {
	case SKSE::MessagingInterface::kPostLoad:
		ManaToStamina::Watcher::GetSingleton().Install();
		break;
	case SKSE::MessagingInterface::kNewGame:
	case SKSE::MessagingInterface::kPostLoadGame:
		// Reinstall on load
		ManaToStamina::Watcher::GetSingleton().Install();
		break;
	}
}

// Simple: we need to handle power spell cast? Easier: use event handling
// But for now, also allow console command style via Papyrus? Maybe better to hook
// Alternatively, just add a spell and handle via OnSpellCast - but need event sink
// Let us use MagicCaster events if available via SKSE; simpler: poll for spell cast is heavy
// Better approach: register for spell cast event. But need to implement event sink

extern "C" DLLEXPORT bool SKSEAPI SKSEPlugin_Query(const SKSE::QueryInterface* a_skse, SKSE::PluginInfo* a_info)
{
	a_info->infoVersion = SKSE::PluginInfo::kVersion;
	a_info->name = "RFABManaToStamina";
	a_info->version = 1;

	if (a_skse->IsEditor()) {
		return false;
	}

	const auto ver = a_skse->RuntimeVersion();
	if (ver < SKSE::RUNTIME_1_5_97) {
		log::critical("unsupported runtime v{}", ver.string());
		return false;
	}

	return true;
}

extern "C" DLLEXPORT bool SKSEAPI SKSEPlugin_Load(const SKSE::QueryInterface* a_skse)
{
	Init(a_skse);
	DebugLog::InitLogging("RFABManaToStamina");

	const auto messaging = SKSE::GetMessagingInterface();
	if (!messaging->RegisterListener("SKSE", SKSEMessageHandler)) {
		log::critical("could not register messaging listener");
		return false;
	}

	ManaToStamina::Watcher::GetSingleton().OnAttach(GetModuleHandle("RFABManaToStamina.dll"));
	ManaToStamina::Watcher::GetSingleton().SetupSerialization();

	log::info("RFABManaToStamina loaded");
	return true;
}
