picosystem_executable(
    tomodachi
    tomodachi.cpp
)

# flash access for saving the pal between power-offs
target_link_libraries(tomodachi hardware_flash)

disable_startup_logo(tomodachi)
