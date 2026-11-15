target("EnderGameFramework")
	set_kind("headeronly")
	add_headerfiles("EGF/*.hpp")
	add_includedirs(".", { public = true })

target_end()