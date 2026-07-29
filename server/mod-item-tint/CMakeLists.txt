set(mod_item_tint_sources
  src/loader.cpp
  src/ItemTint.cpp
)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src)

add_library(modules.mod-item-tint INTERFACE)
target_sources(modules.mod-item-tint INTERFACE ${mod_item_tint_sources})
target_link_libraries(modules.mod-item-tint INTERFACE ac-worldserver)

message("  -> Prepared: mod-item-tint")
