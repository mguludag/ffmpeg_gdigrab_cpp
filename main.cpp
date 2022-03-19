#include <thread>
#include <chrono>
#include <fmt/core.h>
#include "screenrecordimpl.h"

constexpr auto endline = "\n";

int main(int argc, char *argv[])
{
    if(argc > 6){
        ScreenRecordImpl sr(argv[1],std::atoi(argv[2]),std::atoi(argv[3]),std::atoi(argv[4]),std::atoi(argv[5]));
        sr.start();
        std::this_thread::sleep_for(std::chrono::seconds(std::atoi(argv[6]) == 0 ? std::numeric_limits<uint32_t>::max() : std::atoi(argv[6])));
    }
    else {
        fmt::print("USAGE{}this_exe outfile primaryscr_w primaryscr_h fps quality(1 - 31) duration(seconds(0 is infinite))",endline);
    }

}
