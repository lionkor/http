project(http)

if (MSVC)
    add_compile_options(/W4 /WX)
else()
    add_compile_options(-Wall -Wextra -pedantic -O2 -g -flto)
endif()

add_executable(http-server 
    src/main.c
    include/http_server.h src/http_server.c
    include/error_t.h src/error_t.c
    include/memory.h src/memory.c)

target_include_directories(http-server PRIVATE include)
