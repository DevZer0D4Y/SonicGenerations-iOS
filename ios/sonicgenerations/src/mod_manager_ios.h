#pragma once
#include <filesystem>
#include <functional>

// The mod manager (src/mods/mod_database.h): import, enable, order. Shows
// over the game window and calls done once the player taps Play (Done when
// in_game), after the selection is saved. Mods load when the game starts, so
// in game a changed selection offers to quit and apply it on the next launch.
void SgRunModManager(std::filesystem::path root, std::function<void()> done, bool in_game);

