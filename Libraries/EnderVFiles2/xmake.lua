target("EnderVFiles2")
    set_kind("static")
    add_headerfiles("EnderVFiles2.hpp")
    add_files("*.cpp")
    add_includedirs(".", { public = true })
    
target_end()