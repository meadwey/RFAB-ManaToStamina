#pragma once

#include <Windows.h>

namespace ManaToStamina
{
	namespace Config
	{
		// Defaults for RFABManaToStamina.ini, which lives next to the DLL
		// (Data\SKSE\Plugins\RFABManaToStamina.ini) and is re-read on every
		// save load.
		inline constexpr auto kTogglePowerEditorIDDefault{ "RFABManaToStamina_Toggle" };

		// The float global RFAB keeps the total per-second mana upkeep in.
		inline constexpr auto kManaDrainUpkeepGlobalDefault{ "RFAB_Global_ManaDrain_Upkeep" };

		// A magic effect that always exists with RFAB.esp and is only used as
		// a harmless placeholder on the toggle power (magnitude 0), so the
		// power is a valid, castable spell that raises a spell-cast event.
		inline constexpr auto kPlaceholderEffectDefault{ "RFAB_Effect_ManaDrain" };
	}

	class Watcher
	{
	public:
		[[nodiscard]] static Watcher& GetSingleton();

		// Remembers the module handle so the ini can be found next to the
		// DLL; must run from DllMain on process attach.
		void OnAttach(HMODULE a_module);

		// Registers the cosave callbacks; must run from SKSEPluginLoad.
		void SetupSerialization();

		// Called on kPostLoadGame / kNewGame.
		void Install();

		// Called from DllMain on process detach.
		void Unload();

		// Flips the redirect on/off and shows a HUD message.
		void Toggle();

	private:
		Watcher() = default;
	};
}
