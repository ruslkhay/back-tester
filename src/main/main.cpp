// main function for the back-tester app
// please, keep it minimalistic

#include "PresentToUser.hpp"
#include "common/MarketDataEvent.hpp"
#include "common/MarketDataParser.hpp"

#include <deque>
#include <fstream>
#include <iostream>
#include <vector>

int main(int argc, const char* argv[])
{
    if (argc < 2)
    {
        std::cerr << "Usage: back-tester <path_to_ndjson_file>\n";
        return 1;
    }

    std::ifstream file(argv[1]);
    if (!file.is_open())
    {
        std::cerr << "Error: could not open " << argv[1] << "\n";
        return 1;
    }

    PresentToUser(std::cout, file);
    return 0;
}
