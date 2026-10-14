#pragma once

#include <Windows.h>

namespace ManaToStamina
{
	class Watcher
	{
	public:
		[[nodiscard]] static Watcher& GetSingleton();

		void OnAttach(HMODULE a_module);
		void SetupSerialization();
		void Install();
		void Unload();
		void Toggle();

	private:
		Watcher() = default;
	};
}
