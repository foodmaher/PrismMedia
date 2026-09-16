#include "../PrismTextureStreamerFB/diagnostic_options.h"
#include <cassert>
#include <iostream>

int main()
{
    uint64_t n{};
    assert(diagnostic_options::number("0x2A62BF", n) && n == 0x2A62BF);
    assert(diagnostic_options::number("18446744073709551615", n));
    for (const auto* bad : {"", "-1", "+1", "0x", "1junk", "1e3", "18446744073709551616"})
        assert(!diagnostic_options::number(bad,n));
    for (const auto* bad : {"test 0", "test 31", "test 1 slot=128", "test 1 every=0",
        "test 1 slot=6 slot=7", "test 1 budget=-1", "test 1 limit=513", "test 1 unknown=0",
        "test 1 caller=garbage", "test 1 width=16385", "test 1 extra", "bad/label 2"}) {
        diagnostic_options::capture c; std::string error; std::istringstream input(bad);
        assert(!diagnostic_options::parse(input,c,error) && !error.empty());
    }
    diagnostic_options::capture c; std::string error;
    std::istringstream good("native-off 30 slot=127 every=7 limit=512 budget=100000 caller=0x2A62BF resource=0x1234");
    assert(diagnostic_options::parse(good,c,error));
    assert(c.slot==127 && c.every==7 && c.limit==512 && c.caller==0x2A62BF && c.resource==0x1234);
    std::cout << "Capture options tests passed\n";
}
