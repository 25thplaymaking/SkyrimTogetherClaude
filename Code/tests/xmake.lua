
target("TPTests")
    set_kind("binary")
    set_group("Tests")
    add_includedirs(
        ".", "../encoding", "../server")
    add_headerfiles("**.h")
    add_files("*.cpp")
    -- Compiled straight into the test binary rather than linked from STServer.
    -- STServer is a shared library that drags in entt, sol2, GameNetworkingSockets
    -- and the server PCH. NetworkMetrics is deliberately free of all of that so it
    -- unit tests standalone -- if this file ever needs a server header, that
    -- isolation has leaked and the dependency should be removed, not accommodated.
    add_files("../server/Metrics/NetworkMetrics.cpp")
    add_deps("SkyrimEncoding")
    add_packages(
        "tiltedcore",
        "hopscotch-map",
        "catch2",
        "mimalloc",
        "glm")
