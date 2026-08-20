#include <EngineExt/AdaptiveMusic.hpp>

namespace EnderEngine::Extensions::AdaptiveMusic {
	Player* AdaptiveMusicExtension::createPlayer() const {
		return new Player();
	}

	void AdaptiveMusicExtension::destroyPlayer(Player* player) const {
		if (player) {
			delete player;
		}
	}

	HighLevelController* AdaptiveMusicExtension::createHLController() const {
		return new HighLevelController();
	}

	void AdaptiveMusicExtension::destroyHLController(HighLevelController* controller) const {
		if (controller) {
			delete controller;
		}
	}

} // namespace EnderEngine::Extensions::AdaptiveMusic

EEEXT_A0_API void* EE_EXT_GETEXTPTRFUNCNAME() {
	static EnderEngine::Extensions::AdaptiveMusic::AdaptiveMusicExtension instance;
	return &instance;
}