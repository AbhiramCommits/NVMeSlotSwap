/*
 * main.cpp - NVMeSlotSwap command line interface.
 */
#include <cstdio>
#include <cstring>

#include "nvme/driver.hpp"

namespace {

void print_usage(FILE *out)
{
    std::fprintf(out,
                 "NVMeSlotSwap - non-disruptive dual-image NVMe firmware activation\n"
                 "\n"
                 "Usage: nvmeslotswap <command> [options]\n"
                 "\n"
                 "Commands:\n"
                 "  info                 Show the emulated controller summary\n"
                 "  ident                Identify Controller / Identify Namespace\n"
                 "  io [options]         Run a smoke I/O workload\n"
                 "                         --queues N --depth N --seconds N --rw PCT\n"
                 "  fw list              Read the Firmware Slot Information log (0x03)\n"
                 "  fw stage             Download a firmware image in 4 KiB chunks\n"
                 "                         --slot N --rev STR --size BYTES\n"
                 "  fw commit            Commit a staged image\n"
                 "                         --slot N --action 0|1|2|3 [--fail-commit]\n"
                 "  swap-test [options]  Continuous I/O across activation, JSON result\n"
                 "                         --queues N --depth N --warmup-ms N --post-ms N\n"
                 "                         --action 0|1|2|3|none --rw PCT\n"
                 "                         [--json PATH] [--fail-commit]\n"
                 "                         [--slot N] [--rev STR] [--size BYTES]\n"
                 "  --help               Show this help\n");
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        print_usage(stderr);
        return 2;
    }
    const char *cmd = argv[1];
    if (std::strcmp(cmd, "--help") == 0 || std::strcmp(cmd, "-h") == 0 ||
        std::strcmp(cmd, "help") == 0) {
        print_usage(stdout);
        return 0;
    }
    std::fprintf(stderr, "nvmeslotswap: '%s' is not implemented yet\n", cmd);
    return 2;
}
