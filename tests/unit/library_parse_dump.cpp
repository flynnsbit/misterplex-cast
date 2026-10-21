// Dumps what library_browser.hpp parseLibraryXml returns for each XML file, one row per line:
// D|V <tab> title <tab> key <tab> ratingKey <tab> type. Driver for test_library_parse_corpus.py.
// (Tracks print as V, as the daemon treats them: directory=false.)
#include "libmisterplex/library_browser.hpp"

#include <fstream>
#include <iostream>
#include <sstream>

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        std::ifstream f(argv[i]);
        std::stringstream ss;
        ss << f.rdbuf();
        std::cout << "FILE\t" << argv[i] << "\n";
        for (const auto& r : misterplex::parseLibraryXml(ss.str()))
            std::cout << (r.directory ? "D" : "V") << "\t" << r.title << "\t" << r.key << "\t" << r.ratingKey
                      << "\t" << r.type << "\n";
    }
    return 0;
}
