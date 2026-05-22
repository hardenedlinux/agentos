#include <gtest/gtest.h>
#include <sys/wait.h>
#include <unistd.h>
#include "agentos/sandbox.h"

namespace agentos {

TEST(SandboxTest, ApplySandboxFailsWithoutCgroup) {
    pid_t pid = fork();
    ASSERT_GE(pid, 0) << "fork failed";

    if (pid == 0) {
        // Child process: run sandbox
        std::vector<std::string> read_paths;
        std::vector<std::string> write_paths;
        std::vector<int> tcp_ports;
        bool result = apply_sandbox("/tmp/nonexistent", read_paths, write_paths, tcp_ports);
        // Exit with 0 if sandbox succeeded (unexpected), 1 if it failed (expected)
        _exit(result ? 0 : 1);
    }

    // Parent process: wait for child
    int status;
    waitpid(pid, &status, 0);

    // Child should have exited normally with code 1 (sandbox failed)
    EXPECT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 1);
}

} // namespace agentos
