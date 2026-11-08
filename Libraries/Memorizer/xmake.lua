target("Memorizer")
    set_kind("headeronly")
    add_headerfiles("Memorizer/*.hpp")
    add_includedirs(".", { public = true })
    add_includedirs("../../Include") -- Engine Headers

target_end()

target("MemorizerBenchmark")
    set_kind("binary")
    add_deps("Memorizer")
    add_files("Benchmark.cpp")

target_end()