#include "agentos/sandbox.h"
#include "agentos/home_init.h"
#include "agentos/database/database.h"
#include "agentos/rpc.h"
#include <spdlog/spdlog.h>
#include <sys/capability.h>
#include <seccomp.h>
#include <linux/landlock.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>

namespace agentos {

// Helper: write PID to cgroup.procs
static bool join_cgroup(const std::string& cgroup_path) {
    std::ofstream procs(cgroup_path + "/cgroup.procs");
    if (!procs.is_open()) {
        spdlog::warn("[sandbox] cannot open cgroup.procs at {}", cgroup_path);
        return false;
    }
    procs << getpid();
    return true;
}

// Helper: apply seccomp whitelist
static bool apply_seccomp() {
    scmp_filter_ctx ctx = seccomp_init(SCMP_ACT_KILL);
    if (!ctx) {
        spdlog::error("[sandbox] seccomp_init failed");
        return false;
    }

    // Allow basic syscalls needed for typical execution
    int syscalls[] = {
        SCMP_SYS(read), SCMP_SYS(write), SCMP_SYS(open), SCMP_SYS(close),
        SCMP_SYS(mmap), SCMP_SYS(munmap), SCMP_SYS(mprotect),
        SCMP_SYS(brk), SCMP_SYS(exit_group), SCMP_SYS(exit),
        SCMP_SYS(fstat), SCMP_SYS(lseek), SCMP_SYS(ioctl),
        SCMP_SYS(writev), SCMP_SYS(readv),
        SCMP_SYS(clock_gettime), SCMP_SYS(getrandom),
        SCMP_SYS(getpid), SCMP_SYS(gettid),
        SCMP_SYS(futex), SCMP_SYS(nanosleep),
        SCMP_SYS(dup), SCMP_SYS(dup2),
        SCMP_SYS(pipe), SCMP_SYS(pipe2),
        SCMP_SYS(clone), SCMP_SYS(fork), SCMP_SYS(vfork),
        SCMP_SYS(execve), SCMP_SYS(execveat),
        SCMP_SYS(access), SCMP_SYS(faccessat),
        SCMP_SYS(stat), SCMP_SYS(lstat),
        SCMP_SYS(readlink), SCMP_SYS(readlinkat),
        SCMP_SYS(uname), SCMP_SYS(sched_yield),
        SCMP_SYS(getcwd), SCMP_SYS(chdir), SCMP_SYS(fchdir),
        SCMP_SYS(mkdir), SCMP_SYS(mkdirat),
        SCMP_SYS(rmdir), SCMP_SYS(unlink), SCMP_SYS(unlinkat),
        SCMP_SYS(rename), SCMP_SYS(renameat),
        SCMP_SYS(link), SCMP_SYS(symlink),
        SCMP_SYS(getdents), SCMP_SYS(getdents64),
        SCMP_SYS(ftruncate), SCMP_SYS(truncate),
        SCMP_SYS(fcntl), SCMP_SYS(flock),
        SCMP_SYS(sendfile), SCMP_SYS(copy_file_range),
        SCMP_SYS(prctl), SCMP_SYS(arch_prctl),
        SCMP_SYS(set_tid_address), SCMP_SYS(set_robust_list),
        SCMP_SYS(rt_sigaction), SCMP_SYS(rt_sigprocmask),
        SCMP_SYS(rt_sigreturn), SCMP_SYS(sigaltstack),
        SCMP_SYS(madvise), SCMP_SYS(mlock),
        SCMP_SYS(munlock), SCMP_SYS(mincore),
        SCMP_SYS(getegid), SCMP_SYS(geteuid),
        SCMP_SYS(getgid), SCMP_SYS(getuid),
        SCMP_SYS(getresgid), SCMP_SYS(getresuid),
        SCMP_SYS(getgroups), SCMP_SYS(setgroups),
        SCMP_SYS(setgid), SCMP_SYS(setuid),
        SCMP_SYS(setresgid), SCMP_SYS(setresuid),
        SCMP_SYS(setpgid), SCMP_SYS(getpgid),
        SCMP_SYS(setsid), SCMP_SYS(getsid),
        SCMP_SYS(wait4), SCMP_SYS(waitpid),
        SCMP_SYS(kill), SCMP_SYS(tkill),
        SCMP_SYS(socket), SCMP_SYS(connect),
        SCMP_SYS(bind), SCMP_SYS(listen),
        SCMP_SYS(accept), SCMP_SYS(accept4),
        SCMP_SYS(setsockopt), SCMP_SYS(getsockopt),
        SCMP_SYS(sendto), SCMP_SYS(recvfrom),
        SCMP_SYS(sendmsg), SCMP_SYS(recvmsg),
        SCMP_SYS(shutdown), SCMP_SYS(getpeername),
        SCMP_SYS(getsockname),
        SCMP_SYS(epoll_create), SCMP_SYS(epoll_ctl),
        SCMP_SYS(epoll_wait), SCMP_SYS(eventfd),
        SCMP_SYS(timerfd_create), SCMP_SYS(timerfd_settime),
        SCMP_SYS(signalfd),
        SCMP_SYS(newfstatat), SCMP_SYS(statx),
        SCMP_SYS(getcpu), SCMP_SYS(sched_getaffinity),
        SCMP_SYS(sched_setaffinity),
        SCMP_SYS(get_mempolicy), SCMP_SYS(mbind),
        SCMP_SYS(migrate_pages),
        SCMP_SYS(landlock_create_ruleset),
        SCMP_SYS(landlock_add_rule),
        SCMP_SYS(landlock_restrict_self),
    };

    for (int syscall : syscalls) {
        if (seccomp_rule_add(ctx, SCMP_ACT_ALLOW, syscall, 0) != 0) {
            spdlog::error("[sandbox] seccomp_rule_add failed for syscall {}", syscall);
            seccomp_release(ctx);
            return false;
        }
    }

    if (seccomp_load(ctx) != 0) {
        spdlog::error("[sandbox] seccomp_load failed");
        seccomp_release(ctx);
        return false;
    }

    seccomp_release(ctx);
    return true;
}

// Helper: unshare namespaces
static bool apply_namespaces() {
    // Unshare network namespace
    if (unshare(CLONE_NEWNET) != 0) {
        spdlog::error("[sandbox] unshare CLONE_NEWNET failed: {}", strerror(errno));
        return false;
    }
    // Unshare mount namespace
    if (unshare(CLONE_NEWNS) != 0) {
        spdlog::error("[sandbox] unshare CLONE_NEWNS failed: {}", strerror(errno));
        return false;
    }
    return true;
}

// Helper: set up tmpfs and bind mounts
static bool setup_mounts(const std::string& job_dir,
                         const std::vector<std::string>& allowed_read_paths,
                         const std::vector<std::string>& allowed_write_paths) {
    // Mount tmpfs for workspace
    if (mount("tmpfs", job_dir.c_str(), "tmpfs", 0, "size=64M") != 0) {
        spdlog::error("[sandbox] mount tmpfs on {} failed: {}", job_dir, strerror(errno));
        return false;
    }

    // Bind mount allowed read paths read-only
    for (const auto& path : allowed_read_paths) {
        std::string target = job_dir + "/read/" + path;
        // Create target directory
        mkdir(target.c_str(), 0755);
        if (mount(path.c_str(), target.c_str(), "none", MS_BIND | MS_RDONLY, nullptr) != 0) {
            spdlog::warn("[sandbox] bind mount {} read-only failed: {}", path, strerror(errno));
        }
    }

    // Bind mount allowed write paths read-write
    for (const auto& path : allowed_write_paths) {
        std::string target = job_dir + "/write/" + path;
        mkdir(target.c_str(), 0755);
        if (mount(path.c_str(), target.c_str(), "none", MS_BIND, nullptr) != 0) {
            spdlog::warn("[sandbox] bind mount {} read-write failed: {}", path, strerror(errno));
        }
    }

    return true;
}

// Helper: apply Landlock v4 restrictions
static bool apply_landlock(const std::vector<std::string>& allowed_read_paths,
                           const std::vector<std::string>& allowed_write_paths,
                           const std::vector<int>& allowed_tcp_ports) {
    // Create ruleset
    struct landlock_ruleset_attr rs_attr = {
        /*handled_access_fs=*/
            LANDLOCK_ACCESS_FS_READ_FILE |
            LANDLOCK_ACCESS_FS_WRITE_FILE |
            LANDLOCK_ACCESS_FS_READ_DIR |
            LANDLOCK_ACCESS_FS_REMOVE_FILE |
            LANDLOCK_ACCESS_FS_REMOVE_DIR |
            LANDLOCK_ACCESS_FS_MAKE_CHAR |
            LANDLOCK_ACCESS_FS_MAKE_DIR |
            LANDLOCK_ACCESS_FS_MAKE_REG |
            LANDLOCK_ACCESS_FS_MAKE_SOCK |
            LANDLOCK_ACCESS_FS_MAKE_FIFO |
            LANDLOCK_ACCESS_FS_MAKE_BLOCK |
            LANDLOCK_ACCESS_FS_MAKE_SYM,
        /*handled_access_net=*/
            LANDLOCK_ACCESS_NET_BIND_TCP |
            LANDLOCK_ACCESS_NET_CONNECT_TCP,
    };

    int rs_fd = syscall(SYS_landlock_create_ruleset, &rs_attr, sizeof(rs_attr), 0);
    if (rs_fd < 0) {
        spdlog::error("[sandbox] landlock_create_ruleset failed: {}", strerror(errno));
        return false;
    }

    // Add allowed read paths
    for (const auto& path : allowed_read_paths) {
        struct landlock_path_beneath_attr path_attr = {};
        path_attr.allowed_access = LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR;
        path_attr.parent_fd = open(path.c_str(), O_PATH | O_CLOEXEC);
        if (path_attr.parent_fd < 0) {
            spdlog::warn("[sandbox] open {} for landlock failed: {}", path, strerror(errno));
            continue;
        }
        if (syscall(SYS_landlock_add_rule, rs_fd, LANDLOCK_RULE_PATH_BENEATH,
                    &path_attr, 0) != 0) {
            spdlog::warn("[sandbox] landlock_add_rule for {} failed: {}", path, strerror(errno));
        }
        close(path_attr.parent_fd);
    }

    // Add allowed write paths
    for (const auto& path : allowed_write_paths) {
        struct landlock_path_beneath_attr path_attr = {};
        path_attr.allowed_access = LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_WRITE_FILE |
                                   LANDLOCK_ACCESS_FS_READ_DIR | LANDLOCK_ACCESS_FS_REMOVE_FILE |
                                   LANDLOCK_ACCESS_FS_REMOVE_DIR | LANDLOCK_ACCESS_FS_MAKE_REG |
                                   LANDLOCK_ACCESS_FS_MAKE_DIR;
        path_attr.parent_fd = open(path.c_str(), O_PATH | O_CLOEXEC);
        if (path_attr.parent_fd < 0) {
            spdlog::warn("[sandbox] open {} for landlock failed: {}", path, strerror(errno));
            continue;
        }
        if (syscall(SYS_landlock_add_rule, rs_fd, LANDLOCK_RULE_PATH_BENEATH,
                    &path_attr, 0) != 0) {
            spdlog::warn("[sandbox] landlock_add_rule for {} failed: {}", path, strerror(errno));
        }
        close(path_attr.parent_fd);
    }

    // Add allowed TCP ports
    for (int port : allowed_tcp_ports) {
        struct landlock_net_port_attr net_attr = {};
        net_attr.allowed_access = LANDLOCK_ACCESS_NET_BIND_TCP | LANDLOCK_ACCESS_NET_CONNECT_TCP;
        net_attr.port = static_cast<uint64_t>(port);
        if (syscall(SYS_landlock_add_rule, rs_fd, LANDLOCK_RULE_NET_PORT,
                    &net_attr, 0) != 0) {
            spdlog::warn("[sandbox] landlock_add_rule for port {} failed: {}", port, strerror(errno));
        }
    }

    // Restrict self
    if (syscall(SYS_landlock_restrict_self, rs_fd, 0) != 0) {
        spdlog::error("[sandbox] landlock_restrict_self failed: {}", strerror(errno));
        close(rs_fd);
        return false;
    }

    close(rs_fd);
    return true;
}

// Helper: drop capabilities
static bool drop_capabilities() {
    cap_t caps = cap_get_proc();
    if (!caps) {
        spdlog::error("[sandbox] cap_get_proc failed");
        return false;
    }
    if (cap_clear(caps) != 0) {
        spdlog::error("[sandbox] cap_clear failed");
        cap_free(caps);
        return false;
    }
    if (cap_set_proc(caps) != 0) {
        spdlog::error("[sandbox] cap_set_proc failed");
        cap_free(caps);
        return false;
    }
    cap_free(caps);
    return true;
}

bool apply_sandbox(const std::string& job_dir,
                   const std::vector<std::string>& allowed_read_paths,
                   const std::vector<std::string>& allowed_write_paths,
                   const std::vector<int>& allowed_tcp_ports) {
    spdlog::info("[sandbox] applying sandbox for job_dir={}", job_dir);

    // 1. cgroup v2 (assume cgroup already created by daemon)
    if (!join_cgroup("/sys/fs/cgroup/agentos")) {
        spdlog::warn("[sandbox] cgroup join failed, continuing");
    }

    // 2. seccomp
    if (!apply_seccomp()) {
        spdlog::error("[sandbox] seccomp setup failed");
        return false;
    }

    // 3. namespaces
    if (!apply_namespaces()) {
        spdlog::error("[sandbox] namespace setup failed");
        return false;
    }

    // 4. mount namespace setup
    if (!setup_mounts(job_dir, allowed_read_paths, allowed_write_paths)) {
        spdlog::error("[sandbox] mount setup failed");
        return false;
    }

    // 5. Landlock v4
    if (!apply_landlock(allowed_read_paths, allowed_write_paths, allowed_tcp_ports)) {
        spdlog::error("[sandbox] Landlock setup failed");
        return false;
    }

    // 6. drop capabilities
    if (!drop_capabilities()) {
        spdlog::error("[sandbox] capability drop failed");
        return false;
    }

    spdlog::info("[sandbox] sandbox applied successfully");
    return true;
}

void confine_daemon(const std::filesystem::path& home) {
    struct landlock_ruleset_attr rs_attr = {};
    rs_attr.handled_access_fs =
        LANDLOCK_ACCESS_FS_READ_FILE |
        LANDLOCK_ACCESS_FS_WRITE_FILE |
        LANDLOCK_ACCESS_FS_READ_DIR |
        LANDLOCK_ACCESS_FS_EXECUTE |
        LANDLOCK_ACCESS_FS_REMOVE_FILE |
        LANDLOCK_ACCESS_FS_REMOVE_DIR |
        LANDLOCK_ACCESS_FS_MAKE_CHAR |
        LANDLOCK_ACCESS_FS_MAKE_DIR |
        LANDLOCK_ACCESS_FS_MAKE_REG |
        LANDLOCK_ACCESS_FS_MAKE_SOCK |
        LANDLOCK_ACCESS_FS_MAKE_FIFO |
        LANDLOCK_ACCESS_FS_MAKE_BLOCK |
        LANDLOCK_ACCESS_FS_MAKE_SYM;
    rs_attr.handled_access_net =
        LANDLOCK_ACCESS_NET_BIND_TCP |
        LANDLOCK_ACCESS_NET_CONNECT_TCP;

    int rs_fd = syscall(SYS_landlock_create_ruleset, &rs_attr, sizeof(rs_attr), 0);
    if (rs_fd < 0) {
        spdlog::error("[daemon] landlock_create_ruleset failed: {}", strerror(errno));
        return;
    }

    struct landlock_path_beneath_attr path_attr = {};
    path_attr.allowed_access =
        LANDLOCK_ACCESS_FS_READ_FILE |
        LANDLOCK_ACCESS_FS_WRITE_FILE |
        LANDLOCK_ACCESS_FS_READ_DIR |
        LANDLOCK_ACCESS_FS_EXECUTE |
        LANDLOCK_ACCESS_FS_REMOVE_FILE |
        LANDLOCK_ACCESS_FS_REMOVE_DIR |
        LANDLOCK_ACCESS_FS_MAKE_CHAR |
        LANDLOCK_ACCESS_FS_MAKE_DIR |
        LANDLOCK_ACCESS_FS_MAKE_REG |
        LANDLOCK_ACCESS_FS_MAKE_SOCK |
        LANDLOCK_ACCESS_FS_MAKE_FIFO |
        LANDLOCK_ACCESS_FS_MAKE_BLOCK |
        LANDLOCK_ACCESS_FS_MAKE_SYM;
    path_attr.parent_fd = open(home.c_str(), O_PATH | O_CLOEXEC);
    if (path_attr.parent_fd < 0) {
        spdlog::error("[daemon] open {} failed: {}", home.string(), strerror(errno));
        close(rs_fd);
        return;
    }
    if (syscall(SYS_landlock_add_rule, rs_fd, LANDLOCK_RULE_PATH_BENEATH,
                &path_attr, 0) != 0) {
        spdlog::error("[daemon] landlock_add_rule for {} failed: {}", home.string(), strerror(errno));
        close(path_attr.parent_fd);
        close(rs_fd);
        return;
    }
    close(path_attr.parent_fd);

    struct landlock_net_port_attr net_attr = {};
    net_attr.allowed_access = LANDLOCK_ACCESS_NET_CONNECT_TCP;
    net_attr.port = 443;
    if (syscall(SYS_landlock_add_rule, rs_fd, LANDLOCK_RULE_NET_PORT,
                &net_attr, 0) != 0) {
        spdlog::warn("[daemon] landlock_add_rule for port 443 failed: {}", strerror(errno));
    }

    if (syscall(SYS_landlock_restrict_self, rs_fd, 0) != 0) {
        spdlog::error("[daemon] landlock_restrict_self failed: {}", strerror(errno));
        close(rs_fd);
        return;
    }

    close(rs_fd);
    spdlog::info("[daemon] daemon confined to {}", home.string());
}

bool apply_worker_filesystem(const std::string& worker_id,
                             const std::string& run_id) {
    namespace fs = std::filesystem;
    fs::path home = agentos_home();

    // Create run layer directories
    fs::path layers_dir = home / "layers" / "runs" / run_id;
    fs::path upper_dir = layers_dir / "upper";
    fs::path work_dir  = layers_dir / "work";
    std::error_code ec;
    fs::create_directories(upper_dir, ec);
    if (ec) {
        spdlog::error("[sandbox] cannot create upper dir {}: {}", upper_dir.string(), ec.message());
        return false;
    }
    fs::create_directories(work_dir, ec);
    if (ec) {
        spdlog::error("[sandbox] cannot create work dir {}: {}", work_dir.string(), ec.message());
        return false;
    }

    // Create log directory
    fs::path log_dir = home / "logs" / "runs" / run_id;
    fs::create_directories(log_dir, ec);
    if (ec) {
        spdlog::error("[sandbox] cannot create log dir {}: {}", log_dir.string(), ec.message());
        return false;
    }

    // Build overlayfs mount options
    std::string lower0 = "/";
    std::string lower1 = (home / "layers" / "workers" / worker_id).string();
    // Ensure worker base layer directory exists
    std::error_code ec2;
    fs::create_directories(lower1, ec2);
    if (ec2) {
        spdlog::error("[sandbox] cannot create worker base layer {}: {}", lower1, ec2.message());
        return false;
    }
    std::string options = "lowerdir=" + lower0 + ":" + lower1 +
                          ",upperdir=" + upper_dir.string() +
                          ",workdir=" + work_dir.string();

    // Mount overlayfs on a temporary directory (we'll pivot_root into it)
    fs::path merged = layers_dir / "merged";
    fs::create_directories(merged, ec);
    if (ec) {
        spdlog::error("[sandbox] cannot create merged dir {}: {}", merged.string(), ec.message());
        return false;
    }

    if (mount("overlay", merged.c_str(), "overlay", MS_NODEV, options.c_str()) != 0) {
        spdlog::error("[sandbox] overlay mount failed: {}", strerror(errno));
        return false;
    }

    // Create old_root directory inside merged for pivot_root
    fs::path old_root = merged / "old_root";
    fs::create_directories(old_root, ec);
    if (ec) {
        spdlog::error("[sandbox] cannot create old_root dir {}: {}", old_root.string(), ec.message());
        return false;
    }

    // pivot_root
    if (syscall(SYS_pivot_root, merged.c_str(), old_root.c_str()) != 0) {
        spdlog::error("[sandbox] pivot_root failed: {}", strerror(errno));
        return false;
    }

    // chdir to new root
    if (chdir("/") != 0) {
        spdlog::error("[sandbox] chdir / failed: {}", strerror(errno));
        return false;
    }

    // Bind mount worker persistent directory to /home/agentos
    fs::path worker_persistent = home / "workers" / worker_id;
    fs::create_directories(worker_persistent, ec);
    if (ec) {
        spdlog::warn("[sandbox] cannot create worker persistent dir {}: {}", worker_persistent.string(), ec.message());
    }
    fs::create_directories("/home/agentos", ec);
    if (ec) {
        spdlog::error("[sandbox] cannot create /home/agentos: {}", ec.message());
        return false;
    }
    if (mount(worker_persistent.c_str(), "/home/agentos", "none", MS_BIND, nullptr) != 0) {
        spdlog::error("[sandbox] bind mount {} to /home/agentos failed: {}", worker_persistent.string(), strerror(errno));
        return false;
    }

    spdlog::info("[sandbox] worker filesystem isolation applied for run {}", run_id);
    return true;
}

void gc_run_layers(Database& db) {
    auto runs = db.get_all_worker_runs();
    for (const auto& run : runs) {
        if (run.status != "running") {
            std::error_code ec;
            std::filesystem::remove_all(run.layer_path, ec);
            if (ec) {
                spdlog::warn("[sandbox] GC failed to remove layer {}: {}", run.layer_path, ec.message());
            }
        }
    }
}

void apply_worker_sandbox(const std::string& job_dir,
                          const std::vector<std::string>& fs_read,
                          const std::vector<std::string>& fs_write,
                          const std::vector<int>& tcp_connect_ports,
                          bool network,
                          const std::string& run_id) {
    spdlog::info("[sandbox] applying worker sandbox for job_dir={}", job_dir);

    // ADR-016: Apply filesystem isolation (overlayfs + pivot_root)
    // This must be done before other sandbox steps because it changes the root.
    // The worker_id is derived from job_dir (last component).
    std::string worker_id = std::filesystem::path(job_dir).filename().string();
    if (!apply_worker_filesystem(worker_id, run_id)) {
        spdlog::error("[sandbox] worker filesystem isolation failed");
        return;
    }

    // 1. cgroup v2
    if (!join_cgroup("/sys/fs/cgroup/agentos")) {
        spdlog::warn("[sandbox] cgroup join failed, continuing");
    }

    // 2. mount namespace (always)
    if (unshare(CLONE_NEWNS) != 0) {
        spdlog::error("[sandbox] unshare CLONE_NEWNS failed: {}", strerror(errno));
        return;
    }

    // 3. network namespace (only if network false)
    if (!network) {
        if (unshare(CLONE_NEWNET) != 0) {
            spdlog::error("[sandbox] unshare CLONE_NEWNET failed: {}", strerror(errno));
            return;
        }
    }

    // 4. mount tmpfs and bind mounts
    std::vector<std::string> read_paths = fs_read;
    std::vector<std::string> write_paths = fs_write;
    // Implicit: job_dir read+write
    write_paths.push_back(job_dir);
    // Implicit: skills read
    std::filesystem::path home = agentos_home();
    read_paths.push_back((home / "skills").string());

    if (!setup_mounts(job_dir, read_paths, write_paths)) {
        spdlog::error("[sandbox] mount setup failed");
        return;
    }

    // 5. Landlock ruleset
    if (!apply_landlock(read_paths, write_paths, tcp_connect_ports)) {
        spdlog::error("[sandbox] Landlock setup failed");
        return;
    }

    // 6. seccomp
    if (!apply_seccomp()) {
        spdlog::error("[sandbox] seccomp setup failed");
        return;
    }

    // 7. drop capabilities
    if (!drop_capabilities()) {
        spdlog::error("[sandbox] capability drop failed");
        return;
    }

    spdlog::info("[sandbox] worker sandbox applied successfully");
}

} // namespace agentos
