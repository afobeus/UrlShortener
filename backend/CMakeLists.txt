cmake_minimum_required(VERSION 3.16)
project(url_shortener)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Находим зависимости
find_package(Boost REQUIRED COMPONENTS system)
add_subdirectory(thirdparty/SQLiteCpp)
find_package(Threads REQUIRED)

add_executable(url_shortener main.cpp)

target_link_libraries(url_shortener PRIVATE
        Boost::system
        SQLiteCpp
        Threads::Threads
)

# Для некоторых систем может понадобиться явно указать sqlite3
find_package(SQLite3)
if(SQLite3_FOUND)
    target_link_libraries(url_shortener PRIVATE SQLite::SQLite3)
endif()
