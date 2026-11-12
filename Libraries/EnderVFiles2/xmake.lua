-- EnderVFiles2: game-engine asset archive library.
-- This file is included by the engine root xmake.lua.

target("EnderVFiles2")
	set_kind("static")
	set_languages("c++20")

	if is_plat("windows") then
		set_runtimes(is_mode("debug") and "MDd" or "MD")
	end

	add_includedirs("include", { public = true })
	add_includedirs("include/EnderVFiles2", { private = true })
	add_includedirs("../../ThirdParty/OpenSSL/include", { private = true })
	add_includedirs("../../ThirdParty/zlib/include", { private = true })
	add_linkdirs("../../ThirdParty/OpenSSL/lib")
	add_linkdirs("../../ThirdParty/zlib/lib")

	add_files("src/*.cpp")
	add_headerfiles("include/**.hpp")

	-- Static library consumers inherit these links through xmake's link
	-- inheritance. OpenSSL and zlib are supplied by the parent project.
	add_links("libcrypto")
	if is_mode("debug") then
		add_links("zd")
	else
		add_links("z")
	end

target_end()
