#include "eeprom.h"

#include "disk_writer.h"

#include <cstdio>
#include <cstring>

void Eeprom93C46::load(const std::string& path, bool read_only) {
    path_ = read_only ? "" : path;
    memset(mem_, 0xFF, sizeof(mem_));   // blank EEPROM: all ones
    if (FILE* f = fopen(path.c_str(), "rb")) {
        fread(mem_, 1, sizeof(mem_), f);
        fclose(f);
    }
}

// Every word the game writes: the file goes out from the disk writer's thread
// (the game writes several words in a row, from the game thread)
void Eeprom93C46::save() {
    if (path_.empty()) return;
    disk_replace(path_, std::string(reinterpret_cast<const char*>(mem_), sizeof(mem_)));
}

uint8_t Eeprom93C46::read_do() {
    if (busy_reads_ > 0) {   // after a write: busy for a moment, then ready
        busy_reads_--;
        return 0;
    }
    return do_ ? 1 : 0;
}

void Eeprom93C46::write_pins(uint8_t v) {
    bool cs = v & 0x40, clk = v & 0x80, di = v & 0x20;
    if (!cs) {
        if (cs_ && state_ == State::WriteData && bits_ == 16) execute();
        cs_ = false;
        clk_ = clk;
        state_ = State::Idle;
        do_ = true;
        return;
    }
    if (!cs_) {   // CS rising edge
        cs_ = true;
        state_ = State::Idle;
        bits_ = 0;
    }
    bool rising = clk && !clk_;
    clk_ = clk;
    if (!rising) return;

    switch (state_) {
    case State::Idle:
        if (di) { state_ = State::Command; shift_ = 0; bits_ = 0; }
        break;
    case State::Command:
        shift_ = (shift_ << 1) | (di ? 1 : 0);
        if (++bits_ == 8) {
            op_ = (shift_ >> 6) & 3;
            addr_ = shift_ & 0x3F;
            bits_ = 0;
            shift_ = 0;
            if (op_ == 2) { state_ = State::Read; do_ = false; }   // dummy 0 bit
            else if (op_ == 1 || (op_ == 0 && (addr_ >> 4) == 1)) state_ = State::WriteData;
            else { execute(); state_ = State::Idle; }
        }
        break;
    case State::Read:
        do_ = (mem_[addr_] >> (15 - bits_)) & 1;
        if (++bits_ == 16) { bits_ = 0; addr_ = (addr_ + 1) & 63; }
        break;
    case State::WriteData:
        if (bits_ < 16) { shift_ = (shift_ << 1) | (di ? 1 : 0); bits_++; }
        break;
    }
}

void Eeprom93C46::execute() {
    switch (op_) {
    case 0:
        switch (addr_ >> 4) {
        case 3: write_enabled_ = true; break;                 // EWEN
        case 0: write_enabled_ = false; break;                // EWDS
        case 2: if (write_enabled_) memset(mem_, 0xFF, sizeof(mem_)); busy_reads_ = 2; save(); break;  // ERAL
        case 1: if (write_enabled_) for (auto& m : mem_) m = uint16_t(shift_); busy_reads_ = 2; save(); break;  // WRAL
        }
        break;
    case 1:   // WRITE
        if (write_enabled_) mem_[addr_] = uint16_t(shift_);
        busy_reads_ = 2;
        save();
        break;
    case 3:   // ERASE
        if (write_enabled_) mem_[addr_] = 0xFFFF;
        busy_reads_ = 2;
        save();
        break;
    }
}
