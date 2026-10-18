#pragma once

// Shown in the main menu's corner. Bump it with every release: the last number for fixes, the middle
// one for new features (shared by the PS2 and PSP versions)
#define GAME_VERSION "1.14.1"

// The game data this engine reads. Bump it when the converter changes the data so that an older engine
// or older data stops working: Options > Updates then won't install a release that needs newer data (its
// GitHub notes carry <!-- data N -->, written by publish_release.ps1), since data is built on a PC.
#define GAME_DATA_VERSION 1
