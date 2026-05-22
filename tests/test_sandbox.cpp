#include <gtest/gtest.h>
#include "agentos/sandbox.h"

namespace agentos {

TEST(SandboxTest, ApplySandboxFailsWithoutCgroup) {
    std::vector<std::string> read_paths;
    std::vector<std::string> write_paths;
    std::vector<int> tcp_ports;
    bool result = apply_sandbox("/tmp/nonexistent", read_paths, write_paths, tcp_ports);
    EXPECT_FALSE(result);
}

} // namespace agentos
