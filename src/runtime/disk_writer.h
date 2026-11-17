// Files written by a thread of their own (disk_writer.cpp), so a slow drive
// (an external SSD, a USB stick, an antivirus scanning each new file) never
// holds up the game: the caller only hands over a copy of the bytes.
//
// A replace still waiting for the same file is overwritten by the newer one
// (only the latest content goes out); appends to a file keep their order.
// A file that cannot be written is logged once and skipped.
#pragma once
#include <string>

// The whole file, replaced in one step (a .tmp file, then a rename)
void disk_replace(const std::string& path, std::string data);

// Added to the end of the file (created if missing; its folder is not)
void disk_append(const std::string& path, std::string data);

// Waits until everything handed over so far is written (before the program
// exits), up to a few seconds for a drive that hangs
void disk_flush();
