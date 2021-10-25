project(http)

add_compile_options(-Wall -Wextra -pedantic -O3 -g -pthread -D_GNU_SOURCE)

add_executable(http-server 
    src/main.c
    include/http_server.h src/http_server.c
    include/error_t.h src/error_t.c
    include/memory.h src/memory.c)

target_include_directories(http-server PRIVATE include)
target_link_libraries(http-server pthread)
