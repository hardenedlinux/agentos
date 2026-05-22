#include <gtest/gtest.h>
#include <signal.h>
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

    // Child should have been killed by SIGSYS (signal 31)
    EXPECT_TRUE(WIFSIGNALED(status));
    EXPECT_EQ(WTERMSIG(status), SIGSYS);
}

} // namespace agentos
