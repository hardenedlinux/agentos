#include "agentos/sandbox.h"
#include "agentos/database.h"
#include "agentos/home_init.h"
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <linux/landlock.h>
#include <optional>
#include <sched.h>
#include <seccomp.h>
#include <spdlog/spdlog.h>
#include <sstream>
#include <string>
#include <sys/capability.h>
#include <sys/mount.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <vector>

namespace agentos
{

  // Helper: write PID to cgroup.procs
  static bool join_cgroup (const std::string &cgroup_path)
  {
    std::ofstream procs (cgroup_path + "/cgroup.procs");
    if (!procs.is_open ())
      {
        spdlog::warn ("[sandbox] cannot open cgroup.procs at {}", cgroup_path);
        return false;
      }
    procs << getpid ();
    return true;
  }

  // Detect whether the calling process holds CAP_SYS_ADMIN in its effective
  // capability set. Enterprise deployments grant this via a one-time
  // `setcap cap_sys_admin+ep` on the agentos binary at install time.
  //
  //   true  - "privileged" tier: native overlayfs + pivot_root provides
  //           workers with their own filesystem root (ADR-016).
  //   false - "--unsafe"/community tier (default): no filesystem
  //           namespace at all. Workers see the real host filesystem;
  //           isolation is provided entirely by Landlock path rules
  //           (ADR-015) + Landlock network rules + seccomp + cgroup +
  //           capability drop. This requires no special privileges and is
  //           the path exercised by unit tests.
  bool has_cap_sys_admin ()
  {
    cap_t caps = cap_get_proc ();
    if (!caps)
      return false;

    cap_flag_value_t value = CAP_CLEAR;
    if (cap_get_flag (caps, CAP_SYS_ADMIN, CAP_EFFECTIVE, &value) != 0)
      value = CAP_CLEAR;

    cap_free (caps);
    return value == CAP_SET;
  }

  // Helper: apply seccomp whitelist
  static bool apply_seccomp ()
  {
    scmp_filter_ctx ctx = seccomp_init (SCMP_ACT_KILL);
    if (!ctx)
    {
      spdlog::error ("[sandbox] seccomp_init failed");
      return false;
    }

    // Allow basic syscalls needed for typical execution
    int syscalls[] = {
      SCMP_SYS (read),
      SCMP_SYS (write),
      SCMP_SYS (open),
      SCMP_SYS (close),
      SCMP_SYS (mmap),
      SCMP_SYS (munmap),
      SCMP_SYS (mprotect),
      SCMP_SYS (brk),
      SCMP_SYS (exit_group),
      SCMP_SYS (exit),
      SCMP_SYS (fstat),
      SCMP_SYS (lseek),
      SCMP_SYS (ioctl),
      SCMP_SYS (writev),
      SCMP_SYS (readv),
      SCMP_SYS (clock_gettime),
      SCMP_SYS (getrandom),
      SCMP_SYS (getpid),
      SCMP_SYS (gettid),
      SCMP_SYS (futex),
      SCMP_SYS (nanosleep),
      SCMP_SYS (dup),
      SCMP_SYS (dup2),
      SCMP_SYS (pipe),
      SCMP_SYS (pipe2),
      SCMP_SYS (clone),
      SCMP_SYS (fork),
      SCMP_SYS (vfork),
      SCMP_SYS (execve),
      SCMP_SYS (execveat),
      SCMP_SYS (access),
      SCMP_SYS (faccessat),
      SCMP_SYS (stat),
      SCMP_SYS (lstat),
      SCMP_SYS (readlink),
      SCMP_SYS (readlinkat),
      SCMP_SYS (uname),
      SCMP_SYS (sched_yield),
      SCMP_SYS (getcwd),
      SCMP_SYS (chdir),
      SCMP_SYS (fchdir),
      SCMP_SYS (mkdir),
      SCMP_SYS (mkdirat),
      SCMP_SYS (rmdir),
      SCMP_SYS (unlink),
      SCMP_SYS (unlinkat),
      SCMP_SYS (rename),
      SCMP_SYS (renameat),
      SCMP_SYS (link),
      SCMP_SYS (symlink),
      SCMP_SYS (getdents),
      SCMP_SYS (getdents64),
      SCMP_SYS (ftruncate),
      SCMP_SYS (truncate),
      SCMP_SYS (fcntl),
      SCMP_SYS (flock),
      SCMP_SYS (sendfile),
      SCMP_SYS (copy_file_range),
      SCMP_SYS (prctl),
      SCMP_SYS (arch_prctl),
      SCMP_SYS (set_tid_address),
      SCMP_SYS (set_robust_list),
      SCMP_SYS (rt_sigaction),
      SCMP_SYS (rt_sigprocmask),
      SCMP_SYS (rt_sigreturn),
      SCMP_SYS (sigaltstack),
      SCMP_SYS (madvise),
      SCMP_SYS (mlock),
      SCMP_SYS (munlock),
      SCMP_SYS (mincore),
      SCMP_SYS (getegid),
      SCMP_SYS (geteuid),
      SCMP_SYS (getgid),
      SCMP_SYS (getuid),
      SCMP_SYS (getresgid),
      SCMP_SYS (getresuid),
      SCMP_SYS (getgroups),
      SCMP_SYS (setgroups),
      SCMP_SYS (setgid),
      SCMP_SYS (setuid),
      SCMP_SYS (setresgid),
      SCMP_SYS (setresuid),
      SCMP_SYS (setpgid),
      SCMP_SYS (getpgid),
      SCMP_SYS (setsid),
      SCMP_SYS (getsid),
      SCMP_SYS (wait4),
      SCMP_SYS (waitpid),
      SCMP_SYS (kill),
      SCMP_SYS (tkill),
      SCMP_SYS (socket),
      SCMP_SYS (connect),
      SCMP_SYS (bind),
      SCMP_SYS (listen),
      SCMP_SYS (accept),
      SCMP_SYS (accept4),
      SCMP_SYS (setsockopt),
      SCMP_SYS (getsockopt),
      SCMP_SYS (sendto),
      SCMP_SYS (recvfrom),
      SCMP_SYS (sendmsg),
      SCMP_SYS (recvmsg),
      SCMP_SYS (shutdown),
      SCMP_SYS (getpeername),
      SCMP_SYS (getsockname),
      SCMP_SYS (epoll_create),
      SCMP_SYS (epoll_ctl),
      SCMP_SYS (epoll_wait),
      SCMP_SYS (eventfd),
      SCMP_SYS (timerfd_create),
      SCMP_SYS (timerfd_settime),
      SCMP_SYS (signalfd),
      SCMP_SYS (newfstatat),
      SCMP_SYS (statx),
      SCMP_SYS (getcpu),
      SCMP_SYS (sched_getaffinity),
      SCMP_SYS (sched_setaffinity),
      SCMP_SYS (get_mempolicy),
      SCMP_SYS (mbind),
      SCMP_SYS (migrate_pages),
      SCMP_SYS (landlock_create_ruleset),
      SCMP_SYS (landlock_add_rule),
      SCMP_SYS (landlock_restrict_self),
    };

    for (int syscall : syscalls)
    {
      if (seccomp_rule_add (ctx, SCMP_ACT_ALLOW, syscall, 0) != 0)
      {
        spdlog::error ("[sandbox] seccomp_rule_add failed for syscall {}",
                       syscall);
        seccomp_release (ctx);
        return false;
      }
    }

    if (seccomp_load (ctx) != 0)
    {
      spdlog::error ("[sandbox] seccomp_load failed");
      seccomp_release (ctx);
      return false;
    }

    seccomp_release (ctx);
    return true;
  }

  // Helper: apply Landlock v4 restrictions
  static bool
  apply_landlock (const std::vector<std::string> &allowed_read_paths,
                  const std::vector<std::string> &allowed_write_paths,
                  const std::vector<int> &allowed_tcp_ports)
  {
    // Create ruleset
    struct landlock_ruleset_attr rs_attr = {
      /*handled_access_fs=*/
      LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_WRITE_FILE
        | LANDLOCK_ACCESS_FS_READ_DIR | LANDLOCK_ACCESS_FS_REMOVE_FILE
        | LANDLOCK_ACCESS_FS_REMOVE_DIR | LANDLOCK_ACCESS_FS_MAKE_CHAR
        | LANDLOCK_ACCESS_FS_MAKE_DIR | LANDLOCK_ACCESS_FS_MAKE_REG
        | LANDLOCK_ACCESS_FS_MAKE_SOCK | LANDLOCK_ACCESS_FS_MAKE_FIFO
        | LANDLOCK_ACCESS_FS_MAKE_BLOCK | LANDLOCK_ACCESS_FS_MAKE_SYM,
      /*handled_access_net=*/
      LANDLOCK_ACCESS_NET_BIND_TCP | LANDLOCK_ACCESS_NET_CONNECT_TCP,
    };

    int rs_fd
      = syscall (SYS_landlock_create_ruleset, &rs_attr, sizeof (rs_attr), 0);
    if (rs_fd < 0)
    {
      spdlog::error ("[sandbox] landlock_create_ruleset failed: {}",
                     strerror (errno));
      return false;
    }

    // Add allowed read paths
    for (const auto &path : allowed_read_paths)
    {
      struct landlock_path_beneath_attr path_attr = {};
      path_attr.allowed_access
        = LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR;
      path_attr.parent_fd = open (path.c_str (), O_PATH | O_CLOEXEC);
      if (path_attr.parent_fd < 0)
      {
        spdlog::warn ("[sandbox] open {} for landlock failed: {}", path,
                      strerror (errno));
        continue;
      }
      if (syscall (SYS_landlock_add_rule, rs_fd, LANDLOCK_RULE_PATH_BENEATH,
                   &path_attr, 0)
          != 0)
      {
        spdlog::warn ("[sandbox] landlock_add_rule for {} failed: {}", path,
                      strerror (errno));
      }
      close (path_attr.parent_fd);
    }

    // Add allowed write paths
    for (const auto &path : allowed_write_paths)
    {
      struct landlock_path_beneath_attr path_attr = {};
      path_attr.allowed_access
        = LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_WRITE_FILE
          | LANDLOCK_ACCESS_FS_READ_DIR | LANDLOCK_ACCESS_FS_REMOVE_FILE
          | LANDLOCK_ACCESS_FS_REMOVE_DIR | LANDLOCK_ACCESS_FS_MAKE_REG
          | LANDLOCK_ACCESS_FS_MAKE_DIR;
      path_attr.parent_fd = open (path.c_str (), O_PATH | O_CLOEXEC);
      if (path_attr.parent_fd < 0)
      {
        spdlog::warn ("[sandbox] open {} for landlock failed: {}", path,
                      strerror (errno));
        continue;
      }
      if (syscall (SYS_landlock_add_rule, rs_fd, LANDLOCK_RULE_PATH_BENEATH,
                   &path_attr, 0)
          != 0)
      {
        spdlog::warn ("[sandbox] landlock_add_rule for {} failed: {}", path,
                      strerror (errno));
      }
      close (path_attr.parent_fd);
    }

    // Add allowed TCP ports
    for (int port : allowed_tcp_ports)
    {
      struct landlock_net_port_attr net_attr = {};
      net_attr.allowed_access
        = LANDLOCK_ACCESS_NET_BIND_TCP | LANDLOCK_ACCESS_NET_CONNECT_TCP;
      net_attr.port = static_cast<uint64_t> (port);
      if (syscall (SYS_landlock_add_rule, rs_fd, LANDLOCK_RULE_NET_PORT,
                   &net_attr, 0)
          != 0)
      {
        spdlog::warn ("[sandbox] landlock_add_rule for port {} failed: {}",
                      port, strerror (errno));
      }
    }

    // Restrict self
    if (syscall (SYS_landlock_restrict_self, rs_fd, 0) != 0)
    {
      spdlog::error ("[sandbox] landlock_restrict_self failed: {}",
                     strerror (errno));
      close (rs_fd);
      return false;
    }

    close (rs_fd);
    return true;
  }

  // Helper: drop capabilities
  static bool drop_capabilities ()
  {
    cap_t caps = cap_get_proc ();
    if (!caps)
    {
      spdlog::error ("[sandbox] cap_get_proc failed");
      return false;
    }
    if (cap_clear (caps) != 0)
    {
      spdlog::error ("[sandbox] cap_clear failed");
      cap_free (caps);
      return false;
    }
    if (cap_set_proc (caps) != 0)
    {
      spdlog::error ("[sandbox] cap_set_proc failed");
      cap_free (caps);
      return false;
    }
    cap_free (caps);
    return true;
  }

  void confine_daemon (const std::filesystem::path &home)
  {
    struct landlock_ruleset_attr rs_attr = {};
    rs_attr.handled_access_fs
      = LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_WRITE_FILE
        | LANDLOCK_ACCESS_FS_READ_DIR | LANDLOCK_ACCESS_FS_EXECUTE
        | LANDLOCK_ACCESS_FS_REMOVE_FILE | LANDLOCK_ACCESS_FS_REMOVE_DIR
        | LANDLOCK_ACCESS_FS_MAKE_CHAR | LANDLOCK_ACCESS_FS_MAKE_DIR
        | LANDLOCK_ACCESS_FS_MAKE_REG | LANDLOCK_ACCESS_FS_MAKE_SOCK
        | LANDLOCK_ACCESS_FS_MAKE_FIFO | LANDLOCK_ACCESS_FS_MAKE_BLOCK
        | LANDLOCK_ACCESS_FS_MAKE_SYM;
    rs_attr.handled_access_net
      = LANDLOCK_ACCESS_NET_BIND_TCP | LANDLOCK_ACCESS_NET_CONNECT_TCP;

    int rs_fd
      = syscall (SYS_landlock_create_ruleset, &rs_attr, sizeof (rs_attr), 0);
    if (rs_fd < 0)
    {
      spdlog::error ("[daemon] landlock_create_ruleset failed: {}",
                     strerror (errno));
      return;
    }

    struct landlock_path_beneath_attr path_attr = {};
    path_attr.allowed_access
      = LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_WRITE_FILE
        | LANDLOCK_ACCESS_FS_READ_DIR | LANDLOCK_ACCESS_FS_EXECUTE
        | LANDLOCK_ACCESS_FS_REMOVE_FILE | LANDLOCK_ACCESS_FS_REMOVE_DIR
        | LANDLOCK_ACCESS_FS_MAKE_CHAR | LANDLOCK_ACCESS_FS_MAKE_DIR
        | LANDLOCK_ACCESS_FS_MAKE_REG | LANDLOCK_ACCESS_FS_MAKE_SOCK
        | LANDLOCK_ACCESS_FS_MAKE_FIFO | LANDLOCK_ACCESS_FS_MAKE_BLOCK
        | LANDLOCK_ACCESS_FS_MAKE_SYM;
    path_attr.parent_fd = open (home.c_str (), O_PATH | O_CLOEXEC);
    if (path_attr.parent_fd < 0)
    {
      spdlog::error ("[daemon] open {} failed: {}", home.string (),
                     strerror (errno));
      close (rs_fd);
      return;
    }
    if (syscall (SYS_landlock_add_rule, rs_fd, LANDLOCK_RULE_PATH_BENEATH,
                 &path_attr, 0)
        != 0)
    {
      spdlog::error ("[daemon] landlock_add_rule for {} failed: {}",
                     home.string (), strerror (errno));
      close (path_attr.parent_fd);
      close (rs_fd);
      return;
    }
    close (path_attr.parent_fd);

    struct landlock_net_port_attr net_attr = {};
    net_attr.allowed_access = LANDLOCK_ACCESS_NET_CONNECT_TCP;
    net_attr.port = 443;
    if (syscall (SYS_landlock_add_rule, rs_fd, LANDLOCK_RULE_NET_PORT,
                 &net_attr, 0)
        != 0)
    {
      spdlog::warn ("[daemon] landlock_add_rule for port 443 failed: {}",
                    strerror (errno));
    }

    if (syscall (SYS_landlock_restrict_self, rs_fd, 0) != 0)
    {
      spdlog::error ("[daemon] landlock_restrict_self failed: {}",
                     strerror (errno));
      close (rs_fd);
      return;
    }

    close (rs_fd);
    spdlog::info ("[daemon] daemon confined to {}", home.string ());
  }

  // Paths involved in constructing the privileged-mode worker overlay
  // filesystem view (ADR-016, native overlayfs strategy only).
  struct OverlayPaths
  {
    std::string lower0;             // host root, read-only
    std::string lower1;             // worker base layer, read-only
    std::filesystem::path upper;
    std::filesystem::path work;
    std::filesystem::path merged;
  };

  // Create the run-layer directories (upper/work/merged), the log
  // directory, and the worker base layer directory. Returns std::nullopt
  // on any filesystem error (already logged).
  static std::optional<OverlayPaths>
  prepare_overlay_dirs (const std::string &worker_id, const std::string &run_id)
  {
    namespace fs = std::filesystem;
    fs::path home = agentos_home ();
    fs::path layers_dir = home / "layers" / "runs" / run_id;

    OverlayPaths p;
    p.upper  = layers_dir / "upper";
    p.work   = layers_dir / "work";
    p.merged = layers_dir / "merged";

    std::error_code ec;
    fs::create_directories (p.upper, ec);
    if (ec)
    {
      spdlog::error ("[sandbox] cannot create upper dir {}: {}",
                     p.upper.string (), ec.message ());
      return std::nullopt;
    }
    fs::create_directories (p.work, ec);
    if (ec)
    {
      spdlog::error ("[sandbox] cannot create work dir {}: {}",
                     p.work.string (), ec.message ());
      return std::nullopt;
    }
    fs::create_directories (p.merged, ec);
    if (ec)
    {
      spdlog::error ("[sandbox] cannot create merged dir {}: {}",
                     p.merged.string (), ec.message ());
      return std::nullopt;
    }

    // Create log directory
    fs::path log_dir = home / "logs" / "runs" / run_id;
    fs::create_directories (log_dir, ec);
    if (ec)
    {
      spdlog::error ("[sandbox] cannot create log dir {}: {}",
                     log_dir.string (), ec.message ());
      return std::nullopt;
    }

    p.lower0 = "/";
    p.lower1 = (home / "layers" / "workers" / worker_id).string ();
    fs::create_directories (p.lower1, ec);
    if (ec)
    {
      spdlog::error ("[sandbox] cannot create worker base layer {}: {}",
                     p.lower1, ec.message ());
      return std::nullopt;
    }

    return p;
  }

  // Privileged mount strategy (enterprise): native kernel overlayfs.
  //
  // Requires real CAP_SYS_ADMIN in the *current* user namespace at the
  // time of the call. This function must be called BEFORE
  // unshare(CLONE_NEWUSER) — once a new user namespace is created,
  // capability checks are scoped to it, and even a process that held
  // CAP_SYS_ADMIN in init_user_ns beforehand loses the ability to
  // clone_private_mount host-owned mounts (overlay lowerdir on the host
  // root fails with EINVAL "failed to clone lowerpath"). Only
  // unshare(CLONE_NEWNS) (no CLONE_NEWUSER) is required for this path.
  static bool mount_overlay_native (const OverlayPaths &p)
  {
    std::string options = "lowerdir=" + p.lower0 + ":" + p.lower1
                          + ",upperdir=" + p.upper.string ()
                          + ",workdir=" + p.work.string ()
                          + ",userxattr";

    if (mount ("overlay", p.merged.c_str (), "overlay", MS_NODEV,
               options.c_str ())
        != 0)
    {
      spdlog::error ("[sandbox] native overlay mount failed: {}",
                     strerror (errno));
      return false;
    }
    return true;
  }

  // pivot_root into the merged overlay view and bind-mount the worker's
  // persistent directory to /home/agentos (ADR-016 steps 4-5). Shared by
  // both mount strategies.
  static bool pivot_into_overlay (const OverlayPaths &p,
                                  const std::string &worker_id)
  {
    namespace fs = std::filesystem;

    fs::path old_root = p.merged / "old_root";
    std::error_code ec;
    fs::create_directories (old_root, ec);
    if (ec)
    {
      spdlog::error ("[sandbox] cannot create old_root dir {}: {}",
                     old_root.string (), ec.message ());
      return false;
    }

    if (syscall (SYS_pivot_root, p.merged.c_str (), old_root.c_str ()) != 0)
    {
      spdlog::error ("[sandbox] pivot_root failed: {}", strerror (errno));
      return false;
    }

    if (chdir ("/") != 0)
    {
      spdlog::error ("[sandbox] chdir / failed: {}", strerror (errno));
      return false;
    }

    fs::path home = agentos_home ();
    fs::path worker_persistent = home / "workers" / worker_id;
    fs::create_directories (worker_persistent, ec);
    if (ec)
    {
      spdlog::warn ("[sandbox] cannot create worker persistent dir {}: {}",
                    worker_persistent.string (), ec.message ());
    }

    fs::create_directories ("/home/agentos", ec);
    if (ec)
    {
      spdlog::error ("[sandbox] cannot create /home/agentos: {}",
                     ec.message ());
      return false;
    }

    if (mount (worker_persistent.c_str (), "/home/agentos", "none", MS_BIND,
               nullptr)
        != 0)
    {
      spdlog::error ("[sandbox] bind mount {} to /home/agentos failed: {}",
                     worker_persistent.string (), strerror (errno));
      return false;
    }

    // Worker Contract: workers read their persistent storage location from
    // AGENTOS_WORKER_HOME rather than hardcoding /home/agentos, so the same
    // worker code runs unmodified under both the privileged (pivot_root)
    // and --unsafe (real path) filesystem strategies.
    setenv ("AGENTOS_WORKER_HOME", "/home/agentos", 1);

    return true;
  }

  // --unsafe / community strategy: no filesystem namespace at all. The
  // worker sees the real host filesystem; isolation is provided by
  // Landlock (ADR-015) + seccomp + cgroup + capability drop, applied later
  // in apply_worker_sandbox. This function only ensures the worker's
  // persistent directory and the run's log directory exist, and sets
  // AGENTOS_WORKER_HOME to the real path of the worker's persistent
  // directory (see Worker Contract note in pivot_into_overlay above).
  static bool prepare_unsafe_worker_dirs (const std::string &worker_id,
                                          const std::string &run_id)
  {
    namespace fs = std::filesystem;
    fs::path home = agentos_home ();
    std::error_code ec;

    fs::path worker_dir = home / "workers" / worker_id;
    fs::create_directories (worker_dir, ec);
    if (ec)
    {
      spdlog::error ("[sandbox] cannot create worker persistent dir {}: {}",
                     worker_dir.string (), ec.message ());
      return false;
    }

    fs::path log_dir = home / "logs" / "runs" / run_id;
    fs::create_directories (log_dir, ec);
    if (ec)
    {
      spdlog::error ("[sandbox] cannot create log dir {}: {}",
                     log_dir.string (), ec.message ());
      return false;
    }

    // worker_runs.layer_path is NOT NULL; create an (empty) placeholder
    // directory for schema/GC consistency even though --unsafe mode has no
    // overlay layer to store there.
    fs::path layer_dir = home / "layers" / "runs" / run_id;
    fs::create_directories (layer_dir, ec);
    if (ec)
    {
      spdlog::error ("[sandbox] cannot create layer dir {}: {}",
                     layer_dir.string (), ec.message ());
      return false;
    }

    setenv ("AGENTOS_WORKER_HOME", worker_dir.c_str (), 1);
    return true;
  }

  bool apply_worker_filesystem (const std::string &worker_id,
                                const std::string &run_id, bool privileged)
  {
    if (!privileged)
    {
      if (!prepare_unsafe_worker_dirs (worker_id, run_id))
        return false;
      spdlog::info ("[sandbox] worker filesystem prepared for run {} "
                    "(--unsafe: real host filesystem, AGENTOS_WORKER_HOME={})",
                    run_id, std::getenv ("AGENTOS_WORKER_HOME"));
      return true;
    }

    auto paths = prepare_overlay_dirs (worker_id, run_id);
    if (!paths)
      return false;

    if (!mount_overlay_native (*paths))
    {
      spdlog::error ("[sandbox] worker filesystem isolation failed");
      return false;
    }

    if (!pivot_into_overlay (*paths, worker_id))
      return false;

    spdlog::info ("[sandbox] worker filesystem isolation applied for run {} "
                  "(native overlayfs)", run_id);
    return true;
  }


  void gc_run_layers (Database &db)
  {
    auto runs = db.get_all_worker_runs ();
    for (const auto &run : runs)
    {
      if (run.status != WorkerStatus::running)
      {
        std::error_code ec;
        std::filesystem::remove_all (run.layer_path, ec);
        if (ec)
        {
          spdlog::warn ("[sandbox] GC failed to remove layer {}: {}",
                        run.layer_path, ec.message ());
        }
      }
    }
  }

  bool make_mount_namespace_private ()
  {
    // Mount propagation must be made private before any further mount or
    // pivot_root calls, otherwise mount events would propagate back to the
    // host's mount namespace and pivot_root requires a non-shared mount
    // point at the new root.
    if (mount (nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) != 0)
    {
      spdlog::error ("[sandbox] making mount namespace private failed: {}",
                     strerror (errno));
      return false;
    }
    return true;
  }

  bool apply_worker_sandbox (const std::string &job_dir,
                             const std::string &worker_id,
                             const std::vector<std::string> &fs_read,
                             const std::vector<std::string> &fs_write,
                             const std::vector<int> &tcp_connect_ports,
                             bool network, const std::string &run_id)
  {
    spdlog::info ("[sandbox] applying worker sandbox for job_dir={}", job_dir);

    // Two-tier model (ADR-016):
    //
    //   privileged (CAP_SYS_ADMIN present, enterprise `setcap
    //   cap_sys_admin+ep`): unshare(CLONE_NEWNS) + native overlayfs +
    //   pivot_root gives the worker its own filesystem root. CLONE_NEWNET
    //   provides hard network isolation when network:false. Privilege is
    //   used only to construct this jail; capability drop (step 9 below)
    //   removes CAP_SYS_ADMIN etc. before exec — the worker process itself
    //   runs with no special privileges, matching the daemon's normal
    //   (unprivileged) identity.
    //
    //   --unsafe / community (default, no CAP_SYS_ADMIN): no filesystem or
    //   network namespace at all. The worker sees the real host
    //   filesystem; isolation is "directory-level" via Landlock path rules
    //   (job_dir rw, skills/ ro, manifest fs_read/fs_write) plus Landlock
    //   network rules (handled_access_net is always set in
    //   apply_landlock(), so with no tcp_connect_ports — network:false —
    //   ALL TCP bind/connect is denied without needing CLONE_NEWNET). This
    //   requires no special privileges and is the path exercised by unit
    //   tests.
    bool privileged = has_cap_sys_admin ();

    if (privileged)
    {
      spdlog::info ("[sandbox] CAP_SYS_ADMIN present; using native overlayfs "
                    "(privileged mode)");

      // CLONE_NEWNS only — real CAP_SYS_ADMIN in the current (init) user
      // namespace is sufficient and must be retained for the native
      // overlay mount below; CLONE_NEWUSER is not used here (it would
      // scope away CAP_SYS_ADMIN before the overlay mount, causing EINVAL
      // "failed to clone lowerpath").
      if (unshare (CLONE_NEWNS) != 0)
      {
        spdlog::error ("[sandbox] unshare CLONE_NEWNS failed: {}",
                       strerror (errno));
        return false;
      }

      if (!make_mount_namespace_private ())
        return false;

      // overlayfs + pivot_root + bind /home/agentos (ADR-016).
      if (!apply_worker_filesystem (worker_id, run_id, true))
        return false;

      // Network namespace, only if network:false. (Landlock below also
      // denies all TCP in this case; CLONE_NEWNET additionally removes the
      // network stack entirely, per ADR-011.)
      if (!network)
      {
        if (unshare (CLONE_NEWNET) != 0)
        {
          spdlog::error ("[sandbox] unshare CLONE_NEWNET failed: {}",
                         strerror (errno));
          return false;
        }
      }
    }
    else
    {
      spdlog::info ("[sandbox] CAP_SYS_ADMIN absent; using --unsafe "
                    "directory-level isolation");

      if (!apply_worker_filesystem (worker_id, run_id, false))
        return false;

      // No CLONE_NEWNET (would require CAP_SYS_ADMIN without CLONE_NEWUSER).
      // Network isolation for network:false is provided by Landlock below:
      // handled_access_net is always set in apply_landlock(), so with an
      // empty tcp_connect_ports list, all TCP bind/connect is denied.
    }

    // cgroup v2 (ADR-015 step 1). Failure here is non-fatal — resource
    // limits are best-effort; the syscall/Landlock layers are the hard
    // security boundary.
    if (!join_cgroup ("/sys/fs/cgroup/agentos"))
      spdlog::warn ("[sandbox] cgroup join failed, continuing");

    // Landlock (ADR-015). In privileged mode, paths are expressed
    // post-pivot_root (the overlay's lower0=/ makes the original host
    // paths still resolvable at the same paths). In --unsafe mode, paths
    // are real host paths directly — either way the same path strings
    // work. Implicit grants: job_dir rw, skills/ ro. Explicit grants come
    // from the manifest's fs_read/fs_write/tcp_connect_ports.
    std::vector<std::string> read_paths = fs_read;
    std::vector<std::string> write_paths = fs_write;
    write_paths.push_back (job_dir);
    read_paths.push_back ((agentos_home () / "skills").string ());

    if (!apply_landlock (read_paths, write_paths, tcp_connect_ports))
    {
      spdlog::error ("[sandbox] Landlock setup failed");
      return false;
    }

    // seccomp (ADR-015). Must come after every unshare/mount/pivot_root
    // call above — none of those syscalls are in the seccomp whitelist.
    if (!apply_seccomp ())
    {
      spdlog::error ("[sandbox] seccomp setup failed");
      return false;
    }

    // Drop capabilities (ADR-015). In privileged mode this removes
    // CAP_SYS_ADMIN (and everything else) before exec — the jail has
    // already been constructed; the worker process itself runs with no
    // special privileges. In --unsafe mode this is defense-in-depth (a
    // normal unprivileged process typically holds no capabilities anyway).
    if (!drop_capabilities ())
    {
      spdlog::error ("[sandbox] capability drop failed");
      return false;
    }

    spdlog::info ("[sandbox] worker sandbox applied successfully ({})",
                  privileged ? "privileged" : "--unsafe");
    return true;
  }

} // namespace agentos
