#include "HelloWorldExt.hpp"
#include <Engine/Core/Log.hpp>

namespace EnderEngine::Extensions::HelloWorld {

	void HelloWorldExt::onLoad() {
		EInfo("[HelloWorldExt] Hello world from Extension!");
	}

	void HelloWorldExt::onShutdown() {
		EInfo("[HelloWorldExt] Extension is shutting down...");
	}
}

EEEXT_H0_API void* EE_EXT_GETEXTPTRFUNCNAME() {
	static EnderEngine::Extensions::HelloWorld::HelloWorldExt instance;
	return &instance;
}