// Background file writes (src/runtime/disk_writer.cpp): the latest content of
// a replaced file, appends in order, a folder that is missing
#include <cstdio>
#include <string>

#include "disk_writer.h"

static int g_failed = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            printf("FAILED line %d: %s\n", __LINE__, #cond);            \
            g_failed++;                                                 \
        }                                                               \
    } while (0)

static std::string read_file(const char* path) {
    std::string text;
    if (FILE* f = fopen(path, "rb")) {
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
        fclose(f);
    }
    return text;
}

int main() {
    remove("disk_writer_replace.txt");
    remove("disk_writer_append.txt");

    // Replaced many times in a row: the last content is what stays, no .tmp left
    for (int i = 0; i <= 200; i++) disk_replace("disk_writer_replace.txt", "content " + std::to_string(i));
    disk_flush();
    CHECK(read_file("disk_writer_replace.txt") == "content 200");
    CHECK(read_file("disk_writer_replace.txt.tmp").empty());

    // Binary bytes as they are (the EEPROM)
    std::string bytes("\x00\xFF\r\n\x1A", 5);
    disk_replace("disk_writer_replace.txt", bytes);
    disk_flush();
    CHECK(read_file("disk_writer_replace.txt") == bytes);

    // Appends: every line, in order
    std::string expected;
    for (int i = 0; i < 500; i++) {
        std::string line = "line " + std::to_string(i) + "\n";
        disk_append("disk_writer_append.txt", line);
        expected += line;
    }
    disk_flush();
    CHECK(read_file("disk_writer_append.txt") == expected);

    // A folder that does not exist: skipped (logged once), the others still written
    disk_replace("no_such_folder/x.txt", "x");
    disk_append("no_such_folder/y.txt", "y");
    disk_replace("disk_writer_replace.txt", "after");
    disk_flush();
    CHECK(read_file("disk_writer_replace.txt") == "after");

    // Nothing pending: returns at once
    disk_flush();

    remove("disk_writer_replace.txt");
    remove("disk_writer_append.txt");
    if (g_failed) {
        printf("%d checks failed\n", g_failed);
        return 1;
    }
    printf("disk_writer: all checks passed\n");
    return 0;
}
