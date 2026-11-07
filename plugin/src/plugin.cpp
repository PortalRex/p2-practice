// P2 Practice: the server plugin. The engine loads this DLL with plugin_load and calls the callbacks below; the
// plugin takes part in the game only through them, console commands and VScript.
#include <cstring>

#include "engine.h"
#include "sdk.h"
#include "trainer.h"
#include "version.h"

namespace {

class Plugin final : public sdk::IServerPluginCallbacks {
public:
	bool Load(sdk::CreateInterfaceFn interfaceFactory, sdk::CreateInterfaceFn gameServerFactory) override {
		if (loaded_) {
			// A second plugin_load (a cfg that starts with one, with the plugin already up): refused, and the
			// engine answers a refusal with Unload on this same object, which must then leave the loaded
			// plugin alone
			engine::Print("P2 Practice is already loaded.\n");
			refused_ = true;
			return false;
		}
		if (!engine::Init(interfaceFactory, gameServerFactory)) return false;
		trainer::Init();
		loaded_ = true;
		engine::Print("P2 Practice %s loaded. prac_help lists the commands.\n", P2PRACTICE_VERSION);
		return true;
	}

	void Unload() override {
		if (refused_) {
			refused_ = false;
			return;
		}
		if (!loaded_) return;
		trainer::Shutdown();
		engine::Shutdown();
		loaded_ = false;
	}

	void Pause() override {}
	void UnPause() override {}
	const char *GetPluginDescription() override { return "P2 Practice " P2PRACTICE_VERSION; }

	void LevelInit(const char *mapName) override {
		if (loaded_) trainer::OnLevelInit(mapName);
	}
	void ServerActivate(sdk::Edict *, int, int) override {}
	void GameFrame(bool simulating) override {
		if (loaded_) trainer::OnFrame(simulating);
	}
	void LevelShutdown() override {
		if (loaded_) trainer::OnLevelShutdown();
	}

	void ClientActive(sdk::Edict *) override {}
	void ClientFullyConnect(sdk::Edict *) override {}
	void ClientDisconnect(sdk::Edict *) override {}
	void ClientPutInServer(sdk::Edict *, const char *) override {}
	void SetCommandClient(int) override {}
	void ClientSettingsChanged(sdk::Edict *) override {}
	sdk::PluginResult ClientConnect(bool *, sdk::Edict *, const char *, const char *, char *, int) override { return sdk::kPluginContinue; }

	// Commands the partner types reach the host here (their own game doesn't know them and passes them on)
	sdk::PluginResult ClientCommand(sdk::Edict *, const sdk::CCommand &args) override {
		if (loaded_ && args.ArgC() > 0 && !strncmp(args.Arg(0), "prac_", 5) && trainer::OnClientCommand(args)) return sdk::kPluginStop;
		return sdk::kPluginContinue;
	}

	sdk::PluginResult NetworkIDValidated(const char *, const char *) override { return sdk::kPluginContinue; }
	void OnQueryCvarValueFinished(int, sdk::Edict *, int, const char *, const char *) override {}
	void OnEdictAllocated(sdk::Edict *) override {}
	void OnEdictFreed(const sdk::Edict *) override {}

private:
	bool loaded_ = false;
	bool refused_ = false;  // a plugin_load was just refused: the Unload that follows it is not for the loaded plugin
};

Plugin g_plugin;

}  // namespace

// The engine asks the DLL for its plugin object by interface name.
extern "C" __declspec(dllexport) void *CreateInterface(const char *name, int *returnCode) {
	bool match = name && !strcmp(name, sdk::kPluginCallbacks);
	if (returnCode) *returnCode = match ? 0 : 1;
	return match ? static_cast<sdk::IServerPluginCallbacks *>(&g_plugin) : nullptr;
}
