// TLS verify on, shipped CA path, protocol whitelist. No ffmpeg rebuild.
#include "av_open_opts.hpp"
#include "ca_bundle.hpp"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

static std::string slurp(const char* path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static int fail(const char* why) {
    std::cerr << "FAIL " << why << "\n";
    return 1;
}

int main() {
    const std::string wl = misterplex::kAvProtocolWhitelist;
    for (const char* tok : {"file", "http", "https", "tls", "tcp", "crypto"}) {
        if (wl.find(tok) == std::string::npos)
            return fail(tok);
    }
    if (std::string(mpx::kCaBundlePaths[0]) != "/media/fat/linux/misterplex-cacert.pem")
        return fail("ca path");
    const std::string av = slurp("arm/misterplexd/owenb_av_source.cpp");
    const std::string inproc = slurp("arm/misterplexd/av_inproc_decode.cpp");
    for (const std::string* s : {&av, &inproc}) {
        if (s->find("protocol_whitelist") == std::string::npos)
            return fail("whitelist set");
        if (s->find("tls_verify") == std::string::npos)
            return fail("tls_verify");
        if (s->find("ca_file") == std::string::npos)
            return fail("ca_file");
        if (s->find("caBundlePath") == std::string::npos)
            return fail("caBundlePath");
    }
    if (inproc.find("protocol_whitelist") == inproc.rfind("protocol_whitelist"))
        return fail("both inproc opens");
    std::cout << "test_av_open_security: ok\n";
    return 0;
}
