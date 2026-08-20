#pragma once

#ifndef EEEXT_H0_API
# ifdef EEEXT_H0_EXPORTS
#  define EEEXT_H0_API __declspec(dllexport)
# else
#  define EEEXT_H0_API __declspec(dllimport)
# endif
#endif

#include <Engine/Core/Extension.hpp>
#include <Engine/Core/ExtensionAPI.h>

namespace EnderEngine::Extensions::HelloWorld {

	class EEEXT_H0_API HelloWorldExt : public Extension {
	public:
		HelloWorldExt() :
			Extension("HelloWorldExt", Version(0, 1, 0, Guid(UUIDv4::UUID::fromStrFactory("DDB6C275-B552-4CBC-8EA1-46AD48EF7FB9"))), "sally4953") { }
	protected:
		void onLoad() override;
		void onShutdown() override;
	};

} // namespace EnderEngine::Extension::HelloWorld

extern "C" EEEXT_H0_API void* EE_EXT_GETEXTPTRFUNCNAME();