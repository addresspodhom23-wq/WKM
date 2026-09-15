#include <cassert>
#include <fstream>
#include <sstream>
#include <string>

int main() {
    std::ifstream in("src/rendering/renderer.cpp");
    assert(in.good());
    std::ostringstream ss; ss << in.rdbuf();
    const std::string s=ss.str();

    // Both parallel and serial paths must gate weather draw on WMO shelter.
    const std::string gate="weather && camera && !playerSheltered_";
    std::size_t first=s.find(gate);
    assert(first != std::string::npos);
    assert(s.find(gate, first + 1) != std::string::npos);

    // Outdoor WMO groups can still contain a real roof. Physical shelter must
    // therefore include an upward geometry test, while addon indoor state stays
    // on its original group-classification path.
    assert(s.find("segmentBlocked(shelterProbe, roofProbe)") != std::string::npos);
    assert(s.find("playerSheltered_ = insideWmo || roofBlocked") != std::string::npos);

    // Shelter must control the Weather object even if lighting is unavailable.
    const auto state=s.find("weather->setEnabled(!playerSheltered_)");
    const auto lighting=s.find("// Update lighting system");
    assert(state != std::string::npos && lighting != std::string::npos && state < lighting);
    return 0;
}
