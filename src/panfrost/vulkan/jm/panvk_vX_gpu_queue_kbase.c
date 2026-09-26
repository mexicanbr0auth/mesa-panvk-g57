/*
 * PanVK JM backend submit via raw kbase ioctls.
 *
 * Quando PANVK_USE_KBASE=1, esta implementacao substitui a submissao
 * DRM Panfrost por KBASE_IOCTL_JOB_SUBMIT direto em /dev/mali0.
 */

#include "panvk_device.h"
#include "panvk_physical_device.h"
#include "panvk_priv_bo.h"
#include "panvk_queue.h"
#include "panvk_cmd_buffer.h"
#include "decode.h"

#include "lib/kmod/kbase_kmod.h"
#include "lib/kmod/pan_kmod.h"

#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include "util/os_time.h"

#define PANVK_PERF_NOLOG(...) ((void)0)

/* kbase ioctl definitions */
#define KBASE_IOCTL_TYPE 0x80
struct kbase_ioctl_job_submit { uint64_t addr; uint32_t nr_atoms; uint32_t stride; };
#define KBASE_IOCTL_JOB_SUBMIT _IOW(KBASE_IOCTL_TYPE, 2, struct kbase_ioctl_job_submit)

struct base_jd_event_v2 {
   uint32_t event_code;
   uint8_t atom_number;
   uint8_t padding[3];
   uint64_t udata[2];
};

enum {
   BASE_JD_EVENT_DONE = 0x01,
};

/* Implemented here; the only caller (the kbase path of gpu_queue_submit in
 * panvk_vX_gpu_queue.c) prototypes it as panvk_per_arch(kbase_jm_submit). */
VkResult
panvk_per_arch(kbase_jm_submit)(struct vk_queue *vk_queue,
                                struct panvk_gpu_queue *queue,
                                struct panvk_device *dev,
                                struct vk_queue_submit *submit);

/* Waits for the jobs this backend still has in flight, if any. */
static VkResult panvk_kbase_drain(struct panvk_device *dev);

/* kbase CPU syncs are resolved by the wait_many hook when someone waits on
 * them.  Jobs are no longer waited for inside the submit call, so the hook is
 * what turns "the CPU observed the fence" into "the GPU work is really done". */
static VkResult
panvk_jm_kbase_wait_done(void *data,
                         UNUSED const uint64_t targets[PANVK_KBASE_SYNC_TARGET_COUNT],
                         UNUSED uint64_t abs_timeout_ns)
{
   struct panvk_gpu_queue *queue = data;
   struct panvk_device *dev = to_panvk_device(queue->vk.base.device);

   return panvk_kbase_drain(dev);
}

/* base_jd_atom_v2 as understood by this kernel (empirically determined:
 * * KBASE_IOCTL_JOB_SUBMIT requires stride == sizeof(base_jd_atom_v2) and
 *   the only accepted sizes on this kbase are 64 and 72 bytes (the latter
 *   being sizeof + the v2 header's 8 bytes of "seq_nr"-free padding used by
 *   some builds).  The atom lives in *user memory*; the kernel copies it with
 *   copy_from_user and uses .jc as the GPU address of the job chain.
 * * core_req chooses the job slot (BASE_JD_REQ_T -> tiler,
 *   BASE_JD_REQ_FS -> fragment, BASE_JD_REQ_CS -> vertex/compute). */
#define BASE_JD_REQ_FS ((uint32_t)1 << 0)
#define BASE_JD_REQ_CS ((uint32_t)1 << 1)
#define BASE_JD_REQ_T ((uint32_t)1 << 2)
#define BASE_JD_REQ_V ((uint32_t)1 << 4)
#define BASE_JD_REQ_EXTERNAL_RESOURCES ((uint32_t)1 << 8)

#define BASE_EXT_RES_ACCESS_EXCLUSIVE 1ull
#define BASE_EXT_RES_COUNT_MAX 10

struct base_external_resource {
   uint64_t ext_resource;
};
#define BASE_JD_PRIO_MEDIUM 0u

struct base_dependency {
   uint8_t atom_id;
   uint8_t dependency_type;
} __attribute__((packed));

struct base_jd_atom_v2 {
   uint64_t jc;
   uint64_t udata[2];
   uint64_t extres_list;
   uint16_t nr_extres;
   uint8_t jit_id[2];
   struct base_dependency pre_dep[2];
   uint8_t atom_number;
   uint8_t prio;
   uint8_t device_nr;
   uint8_t jobslot;
   uint32_t core_req;
   uint8_t payload[16]; /* padding to 64 bytes */
} __attribute__((packed, aligned(16)));

static VkResult
panvk_kbase_wait_jobs(struct panvk_device *dev,
                     const struct base_jd_atom_v2 *atoms, unsigned count)
{
   static uint64_t perf_calls;
   static uint64_t perf_atoms;
   static int64_t perf_wait_ns;
   static int64_t perf_last_report_ns;

   const int64_t perf_start_ns = os_time_get_nano();
   const unsigned perf_atom_count = count;

   bool pending[256] = { false };
   for (unsigned i = 0; i < count; i++)
      pending[atoms[i].atom_number] = true;

   VkResult result = VK_SUCCESS;
   const int64_t deadline = os_time_get_nano() + 60000000000ll;
   while (count) {
      int64_t remaining = deadline - os_time_get_nano();
      if (remaining <= 0) {
         mesa_loge("kbase: timed out waiting for %u JD atoms", count);
         return VK_ERROR_DEVICE_LOST;
      }

      struct pollfd pfd = { .fd = dev->kmod.dev->fd, .events = POLLIN };
      int ret = poll(&pfd, 1, (remaining + 999999) / 1000000);
      if (ret < 0 && errno == EINTR)
         continue;
      if (ret <= 0 || !(pfd.revents & POLLIN))
         return VK_ERROR_DEVICE_LOST;

      struct base_jd_event_v2 ev;
      ssize_t len = read(dev->kmod.dev->fd, &ev, sizeof(ev));
      if (len < 0 && (errno == EINTR || errno == EAGAIN))
         continue;
      if (len != sizeof(ev)) {
         mesa_loge("kbase: invalid JD event read length %zd", len);
         return VK_ERROR_DEVICE_LOST;
      }

      PANVK_PERF_NOLOG( "PANVKDBG kbase JD event: code=0x%02x atom=%u\n",
              ev.event_code, ev.atom_number);
      if (!pending[ev.atom_number]) {
         mesa_loge("kbase: unexpected JD event for atom %u", ev.atom_number);
         return VK_ERROR_DEVICE_LOST;
      }

      pending[ev.atom_number] = false;
      count--;
      if (ev.event_code != BASE_JD_EVENT_DONE) {
         mesa_loge("kbase: atom %u failed with JD event 0x%02x",
                   ev.atom_number, ev.event_code);
         result = VK_ERROR_DEVICE_LOST;
      }
   }

   const int64_t perf_end_ns = os_time_get_nano();
   perf_calls++;
   perf_atoms += perf_atom_count;
   perf_wait_ns += perf_end_ns - perf_start_ns;

   if (perf_last_report_ns == 0)
      perf_last_report_ns = perf_start_ns;

   if (getenv("PANVK_JM_STATS") &&
       perf_end_ns - perf_last_report_ns >= 1000000000ll) {
      fprintf(stderr,
              "JM_WAIT calls=%llu atoms=%llu total_ms=%.3f avg_us=%.3f\\n",
              (unsigned long long)perf_calls,
              (unsigned long long)perf_atoms,
              (double)perf_wait_ns / 1000000.0,
              perf_calls ? (double)perf_wait_ns /
                              (double)perf_calls / 1000.0
                         : 0.0);

      perf_calls = 0;
      perf_atoms = 0;
      perf_wait_ns = 0;
      perf_last_report_ns = perf_end_ns;
   }

   return result;
}

/*
 * Deferred job completion.
 *
 * The atoms live in stack memory of the submit call, so they are copied here
 * before the ioctl returns and the CPU stops blocking on every job bag.  A
 * window is kept instead of a single outstanding job: the driver's queue lock
 * already serialises submits, so the tracker is a plain per-device list and
 * never needs a lock of its own.
 *
 * The device wide cap is what keeps atom numbers unique.  kbase tracks at most
 * 256 atoms per fd and reports them through the event fd, and the event loop in
 * panvk_kbase_wait_jobs() only tolerates events for atoms it knows about, so
 * ids are handed out monotonically and only recycled after a drain.  The cap
 * (24 batches / 48 atoms) is far below both limits.
 */
/* Off by default: the deferred window still shares pools that the app can
 * recycle between submits, so it stays an opt in until it survives a
 * clean run.  PANVK_JM_ASYNC=1 enables it for an A/B measurement. */
#define PANVK_KBASE_ASYNC_DEFAULT 0
#define PANVK_KBASE_ASYNC_BATCHES 24
#define PANVK_KBASE_ASYNC_ATOMS (2 * PANVK_KBASE_ASYNC_BATCHES)

static struct {
   struct base_jd_atom_v2 atoms[PANVK_KBASE_ASYNC_ATOMS];
   struct panvk_cmd_buffer *cmdbuf;
   unsigned count;
   unsigned batches;
   uint8_t next_id;
   bool failed;
} panvk_kbase_pending;

/* Called by the command buffer reset and destroy paths: the deferred window
 * must be empty before the pools and job chains are recycled. */
void
panvk_per_arch(kbase_jm_drain)(struct panvk_device *dev)
{
   panvk_kbase_drain(dev);
}

static bool
panvk_kbase_async_enabled(void)
{
   const char *env = getenv("PANVK_JM_ASYNC");

   return env ? (strcmp(env, "0") != 0) : (bool)PANVK_KBASE_ASYNC_DEFAULT;
}

/* Waits for every job still in flight.  Safe (and cheap) to call at any point
 * where the CPU is about to read, rewrite or free memory the GPU can touch, or
 * where it needs an ordering guarantee. */
static VkResult
panvk_kbase_drain(struct panvk_device *dev)
{
   if (!panvk_kbase_pending.count)
      return VK_SUCCESS;

   VkResult result = panvk_kbase_wait_jobs(dev, panvk_kbase_pending.atoms,
                                           panvk_kbase_pending.count);

   panvk_kbase_pending.count = 0;
   panvk_kbase_pending.batches = 0;
   panvk_kbase_pending.cmdbuf = NULL;
   panvk_kbase_pending.next_id = 1;
   if (result != VK_SUCCESS)
      panvk_kbase_pending.failed = true;

   return result;
}

static void
panvk_kbase_pending_add(struct panvk_device *dev,
                        struct panvk_cmd_buffer *cmdbuf,
                        const struct base_jd_atom_v2 *atoms, unsigned count)
{
   if (!panvk_kbase_async_enabled()) {
      /* Old behaviour: the caller waits for the job bag right away. */
      panvk_kbase_wait_jobs(dev, atoms, count);
      return;
   }

   assert(panvk_kbase_pending.count + count <=
          ARRAY_SIZE(panvk_kbase_pending.atoms));
   memcpy(&panvk_kbase_pending.atoms[panvk_kbase_pending.count], atoms,
          count * sizeof(atoms[0]));
   panvk_kbase_pending.count += count;
   panvk_kbase_pending.cmdbuf = cmdbuf;
}

static VkResult
panvk_kbase_jm_submit_batch(struct panvk_gpu_queue *queue,
                            struct panvk_cmd_buffer *cmdbuf,
                            struct panvk_batch *batch, uint32_t *bos,
                            unsigned nr_bos, uint32_t *in_fences,
                            unsigned nr_in_fences,
                            bool pipeline,
                            uint8_t *next_atom_id,
                            uint8_t previous_atom,
                            struct base_jd_atom_v2 out_atoms[2],
                            unsigned *out_nr_atoms)
{
   struct panvk_device *dev = to_panvk_device(queue->vk.base.device);
   ASSERTED int ret;

   if (out_nr_atoms)
      *out_nr_atoms = 0;

   PANVK_PERF_NOLOG( "PANVKDBG submit_batch kbase: batch=%p vtc=%s frag=%s bos=%u\n",
           (void *)batch,
           batch->vtc_jc.first_job ? "Y" : "N",
           batch->frag_jc.first_job ? "Y" : "N", nr_bos);

   if (batch->issued) {
      /* GPU writes status/context data into the descriptor pool.
       * Invalidate CPU mappings before restoring descriptors for re-submit. */
      VkResult drain = panvk_kbase_drain(dev);
      if (drain != VK_SUCCESS)
         return drain;

      panvk_pool_invalidate_maps(&cmdbuf->desc_pool);
      pan_kmod_flush_bo_map_syncs(dev->kmod.dev);

      /*
       * Re-submit reset.
       *
       * The first 16 bytes of JOB_HEADER are GPU-written status.
       * On Valhall, MALLOC_VERTEX also writes Draw.Vertex array
       * (descriptor words 34..36). Restore that input state for
       * every MALLOC_VERTEX in the batch, not only first_job.
       */
      util_dynarray_foreach(&batch->jobs, void *, job) {
         uint32_t *j = (uint32_t *)(*job);

#if PAN_ARCH >= 9
         /*
          * MALLOC_VERTEX is Valhall/PAN_ARCH >= 9.
          * Save the type before clearing the GPU-written status.
          */
         uint8_t job_type = (j[4] >> 1) & 0x7f;
#endif

         memset(j, 0, 4 * 4);

#if PAN_ARCH >= 9
         if (job_type == MALI_JOB_TYPE_MALLOC_VERTEX) {
            /*
             * Restore the pristine Draw.Vertex array input.
             * The MALLOC_VERTEX hardware overwrites these words.
             */
            /*
             * MALLOC_VERTEX writes Draw.Vertex Array during execution.
             * Re-pack its pristine input state before re-submitting the
             * recorded command buffer.  At command generation time PanVK
             * initializes only Packet=true; Pointer and both strides are
             * hardware outputs and therefore start at zero.
             */
            struct mali_vertex_array_packed *vertex_array =
               (struct mali_vertex_array_packed *)&j[34];

            pan_pack(vertex_array, VERTEX_ARRAY, cfg) {
               cfg.packet = true;
            }

         }
#endif
      }

      if (batch->tiler.ctx_descs.cpu) {
         memcpy(batch->tiler.heap_desc.cpu, &batch->tiler.heap_templ,
                sizeof(batch->tiler.heap_templ));

         struct mali_tiler_context_packed *ctxs =
            batch->tiler.ctx_descs.cpu;

         for (uint32_t i = 0; i < batch->fb.layer_count; i++)
            memcpy(&ctxs[i], &batch->tiler.ctx_templ, sizeof(*ctxs));
      }

      panvk_pool_flush_maps(&cmdbuf->desc_pool);
   }

   pan_kmod_flush_bo_map_syncs(dev->kmod.dev);

   /* Debug: compare the exact first job before first submit and re-submit. */
   PANVK_PERF_NOLOG(
           "PANVKDBG PRESUB issued=%u batch=%p vtc=%016llx frag=%016llx\\n",
           batch->issued ? 1 : 0, (void *)batch,
           (unsigned long long)batch->vtc_jc.first_job,
           (unsigned long long)batch->frag_jc.first_job);

   if (batch->vtc_jc.first_job) {
      const uint32_t *j =
         (const uint32_t *)(uintptr_t)batch->vtc_jc.first_job;

      PANVK_PERF_NOLOG(
              "PANVKDBG VTC16 "
              "%08x %08x %08x %08x "
              "%08x %08x %08x %08x "
              "%08x %08x %08x %08x "
              "%08x %08x %08x %08x\\n",
              j[0], j[1], j[2], j[3],
              j[4], j[5], j[6], j[7],
              j[8], j[9], j[10], j[11],
              j[12], j[13], j[14], j[15]);
   }

   if (batch->tiler.heap_desc.cpu) {
      const uint32_t *h =
         (const uint32_t *)batch->tiler.heap_desc.cpu;

      const uint32_t *ht =
         (const uint32_t *)&batch->tiler.heap_templ;

      PANVK_PERF_NOLOG(
              "PANVKDBG HEAP gpu=%016llx "
              "%08x %08x %08x %08x "
              "%08x %08x %08x %08x "
              "%08x %08x %08x %08x "
              "%08x %08x %08x %08x\\n",
              (unsigned long long)batch->tiler.heap_desc.gpu,
              h[0], h[1], h[2], h[3],
              h[4], h[5], h[6], h[7],
              h[8], h[9], h[10], h[11],
              h[12], h[13], h[14], h[15]);

      PANVK_PERF_NOLOG(
              "PANVKDBG HEAPT "
              "%08x %08x %08x %08x "
              "%08x %08x %08x %08x "
              "%08x %08x %08x %08x "
              "%08x %08x %08x %08x\\n",
              ht[0], ht[1], ht[2], ht[3],
              ht[4], ht[5], ht[6], ht[7],
              ht[8], ht[9], ht[10], ht[11],
              ht[12], ht[13], ht[14], ht[15]);
   }

   if (batch->tiler.ctx_descs.cpu) {
      const uint32_t *tc =
         (const uint32_t *)batch->tiler.ctx_descs.cpu;

      PANVK_PERF_NOLOG(
              "PANVKDBG TC16 "
              "%08x %08x %08x %08x "
              "%08x %08x %08x %08x "
              "%08x %08x %08x %08x "
              "%08x %08x %08x %08x\\n",
              tc[0], tc[1], tc[2], tc[3],
              tc[4], tc[5], tc[6], tc[7],
              tc[8], tc[9], tc[10], tc[11],
              tc[12], tc[13], tc[14], tc[15]);
   }

   {
      unsigned dbg_idx = 0;

      util_dynarray_foreach(&batch->jobs, void *, job) {
         const uint32_t *w = (const uint32_t *)(*job);

         PANVK_PERF_NOLOG(
                 "PANVKDBG JOB issued=%u idx=%u ptr=%p "
                 "%08x %08x %08x %08x "
                 "%08x %08x %08x %08x "
                 "%08x %08x %08x %08x "
                 "%08x %08x %08x %08x "
                 "%08x %08x %08x %08x "
                 "%08x %08x %08x %08x "
                 "%08x %08x %08x %08x "
                 "%08x %08x %08x %08x\\n",
                 batch->issued ? 1 : 0, dbg_idx++, *job,
                 w[0], w[1], w[2], w[3],
                 w[4], w[5], w[6], w[7],
                 w[8], w[9], w[10], w[11],
                 w[12], w[13], w[14], w[15],
                 w[16], w[17], w[18], w[19],
                 w[20], w[21], w[22], w[23],
                 w[24], w[25], w[26], w[27],
                 w[28], w[29], w[30], w[31]);
      }
   }

   /* A draw chain starts with a MALLOC_VERTEX (Valhall IDVS) job, which
    * needs both the shader cores and the tiler: submitting it with only
    * BASE_JD_REQ_T makes the job fail with JOB_AFFINITY_FAULT (0x44).
    * Compute/NULL chains stay on the vertex/compute slot. */
   uint32_t vtc_core = BASE_JD_REQ_CS | BASE_JD_REQ_T;
   uint32_t frag_core = BASE_JD_REQ_FS;
   if (batch->vtc_jc.first_job) {
      /* Pick the job slot from the first job in the chain: compute (and
       * NULL sync) jobs run on the vertex/compute slot, draw chains on
       * the tiler slot. */
      uint32_t w0 = ((uint32_t *)(uintptr_t)batch->vtc_jc.first_job)[4];
      uint8_t job_type = (w0 >> 1) & 0x7f;
      if (job_type == MALI_JOB_TYPE_COMPUTE || job_type == MALI_JOB_TYPE_NULL)
         vtc_core = BASE_JD_REQ_CS;
   }
   PANVK_PERF_NOLOG( "PANVKDBG core envraw vtc=%s frag=%s\n",
           getenv("PANVK_KBASE_VTC_CORE"), getenv("PANVK_KBASE_FRAG_CORE"));
   if (getenv("PANVK_KBASE_VTC_CORE"))
      vtc_core = strtoul(getenv("PANVK_KBASE_VTC_CORE"), NULL, 0);
   if (getenv("PANVK_KBASE_FRAG_CORE"))
      frag_core = strtoul(getenv("PANVK_KBASE_FRAG_CORE"), NULL, 0);

   if ((!pipeline || getenv("PANVK_SPLIT_MASK")) &&
       getenv("PANVK_SPLIT_MASK") &&
       !kbase_kmod_get_user_buffer_vas(dev->kmod.dev, NULL, 1) &&
        batch->vtc_jc.first_job &&
        batch->frag_jc.first_job) {
      /* This path submits atoms with fixed ids, so the window must be empty
       * before it runs. */
      VkResult drained = panvk_kbase_drain(dev);
      if (drained != VK_SUCCESS)
         return drained;

      /* Run the tiler atom, then mask the polygon-list pointer's tag bits in
       * the tiler context before running the fragment atom. */
      struct base_jd_atom_v2 vatom = {
         .jc = batch->vtc_jc.first_job,
         .atom_number = 1,
         .core_req = vtc_core,
      };
      struct kbase_ioctl_job_submit vsub = {
         .addr = (uint64_t)(uintptr_t)&vatom,
         .nr_atoms = 1,
         .stride = sizeof(vatom),
      };
      ret = pan_kmod_ioctl(dev->kmod.dev->fd, KBASE_IOCTL_JOB_SUBMIT, &vsub);
      if (ret) {
         mesa_loge("kbase: vtc submit failed: %s", strerror(errno));
         return VK_ERROR_DEVICE_LOST;
      }
      VkResult result = panvk_kbase_wait_jobs(dev, &vatom, 1);
      if (result != VK_SUCCESS)
         return result;

      if (batch->tiler.ctx_descs.cpu) {
         panvk_pool_invalidate_maps(&cmdbuf->desc_pool);
         pan_kmod_flush_bo_map_syncs(dev->kmod.dev);
         uint32_t *tc = (uint32_t *)batch->tiler.ctx_descs.cpu;
         uint64_t poly = ((uint64_t)tc[1] << 32) | tc[0];
         uint64_t masked = poly & 0x0000ffffffffffffull;
         tc[0] = (uint32_t)masked;
         tc[1] = (uint32_t)(masked >> 32);
         PANVK_PERF_NOLOG( "PANVKDBG mask poly %016llx -> %016llx\n",
                 (unsigned long long)poly, (unsigned long long)masked);
         panvk_pool_flush_maps(&cmdbuf->desc_pool);
         pan_kmod_flush_bo_map_syncs(dev->kmod.dev);
      }

      struct base_jd_atom_v2 fatom = {
         .jc = batch->frag_jc.first_job,
         .atom_number = 2,
         .core_req = frag_core,
      };
      struct kbase_ioctl_job_submit fsub = {
         .addr = (uint64_t)(uintptr_t)&fatom,
         .nr_atoms = 1,
         .stride = sizeof(fatom),
      };
      ret = pan_kmod_ioctl(dev->kmod.dev->fd, KBASE_IOCTL_JOB_SUBMIT, &fsub);
      if (ret) {
         mesa_loge("kbase: frag submit failed: %s", strerror(errno));
         return VK_ERROR_DEVICE_LOST;
      }
      result = panvk_kbase_wait_jobs(dev, &fatom, 1);
      if (result != VK_SUCCESS)
         return result;
      batch->issued = true;
      return VK_SUCCESS;
   }

   {
      /* Submit the (optional) vertex/tiler chain and the (optional) fragment
       * chain as atoms in a single kbase job bag.  The fragment atom declares
       * a data dependency on the vertex/tiler atom so the scheduler keeps the
       * tiler->fragment ordering the hardware requires. */
      struct base_jd_atom_v2 atoms[2];
      unsigned nr_atoms = 0;

      uint64_t userbuf_vas[BASE_EXT_RES_COUNT_MAX];
      struct base_external_resource extres[BASE_EXT_RES_COUNT_MAX];

      unsigned nr_extres =
         kbase_kmod_get_user_buffer_vas(dev->kmod.dev,
                                        userbuf_vas,
                                        ARRAY_SIZE(userbuf_vas));

      for (unsigned i = 0; i < nr_extres; i++) {
         assert((userbuf_vas[i] & 0xfff) == 0);
         extres[i].ext_resource =
            userbuf_vas[i] | BASE_EXT_RES_ACCESS_EXCLUSIVE;

         PANVK_PERF_NOLOG(
                 "PANVKDBG EXTRES[%u]=%016llx\n",
                 i,
                 (unsigned long long)extres[i].ext_resource);
      }

      memset(atoms, 0, sizeof(atoms));

      uint8_t first_atom_id = 0;
      uint8_t vtc_atom_id = 0;

      if (batch->vtc_jc.first_job) {
         vtc_atom_id = pipeline ? (*next_atom_id)++ : 1;
         first_atom_id = vtc_atom_id;

         atoms[nr_atoms].jc = batch->vtc_jc.first_job;
         atoms[nr_atoms].atom_number = vtc_atom_id;
         atoms[nr_atoms].core_req = vtc_core;

         if (pipeline && previous_atom) {
            atoms[nr_atoms].pre_dep[0].atom_id = previous_atom;
            atoms[nr_atoms].pre_dep[0].dependency_type = 2; /* ORDER */
         }

         if (nr_extres) {
            atoms[nr_atoms].extres_list =
               (uint64_t)(uintptr_t)extres;
            atoms[nr_atoms].nr_extres = nr_extres;
            atoms[nr_atoms].core_req |= BASE_JD_REQ_EXTERNAL_RESOURCES;

            PANVK_PERF_NOLOG(
                    "PANVKDBG VTC EXTRES count=%u core_req=%08x list=%p\n",
                    nr_extres,
                    atoms[nr_atoms].core_req,
                    (void *)extres);
         }

         nr_atoms++;
      }

      if (batch->frag_jc.first_job) {
         uint8_t frag_atom_id = pipeline ? (*next_atom_id)++ : 2;

         if (!first_atom_id)
            first_atom_id = frag_atom_id;

         atoms[nr_atoms].jc = batch->frag_jc.first_job;
         atoms[nr_atoms].atom_number = frag_atom_id;

         if (batch->vtc_jc.first_job) {
            atoms[nr_atoms].pre_dep[0].atom_id = vtc_atom_id;
            atoms[nr_atoms].pre_dep[0].dependency_type = 1; /* DATA */
         } else if (pipeline && previous_atom) {
            atoms[nr_atoms].pre_dep[0].atom_id = previous_atom;
            atoms[nr_atoms].pre_dep[0].dependency_type = 2; /* ORDER */
         }

         atoms[nr_atoms].core_req = frag_core;
         nr_atoms++;
      }

      if (nr_atoms) {
         struct kbase_ioctl_job_submit submit = {
            .addr = (uint64_t)(uintptr_t)atoms,
            .nr_atoms = nr_atoms,
            .stride = sizeof(atoms[0]),
         };

         if (PANVK_DEBUG(TRACE)) {
            VkResult drain = panvk_kbase_drain(dev);
            if (drain != VK_SUCCESS)
               return drain;

            panvk_pool_invalidate_maps(&cmdbuf->desc_pool);
            pan_kmod_flush_bo_map_syncs(dev->kmod.dev);
            if (batch->vtc_jc.first_job)
               pandecode_jc(dev->debug.decode_ctx, batch->vtc_jc.first_job,
                            to_panvk_physical_device(dev->vk.physical)->kmod.dev->props.gpu_id);
            if (batch->frag_jc.first_job)
               pandecode_jc(dev->debug.decode_ctx, batch->frag_jc.first_job,
                            to_panvk_physical_device(dev->vk.physical)->kmod.dev->props.gpu_id);
         }

         ret = pan_kmod_ioctl(dev->kmod.dev->fd, KBASE_IOCTL_JOB_SUBMIT, &submit);
         if (ret) {
            mesa_loge("kbase: KBASE_IOCTL_JOB_SUBMIT failed: %s", strerror(errno));
            return VK_ERROR_DEVICE_LOST;
         }
         PANVK_PERF_NOLOG(
                 "PANVKDBG JD submit ok vtc=%s frag=%s atoms=%u vtc_core=%x frag_core=%x\n",
                 batch->vtc_jc.first_job ? "Y" : "N",
                 batch->frag_jc.first_job ? "Y" : "N", nr_atoms, vtc_core,
                 frag_core);

         if (pipeline) {
            assert(out_atoms && out_nr_atoms);
            memcpy(out_atoms, atoms, nr_atoms * sizeof(atoms[0]));
            *out_nr_atoms = nr_atoms;
         } else {
            VkResult result = panvk_kbase_wait_jobs(dev, atoms, nr_atoms);

            if (result != VK_SUCCESS) {
               panvk_pool_invalidate_maps(&cmdbuf->desc_pool);
               pan_kmod_flush_bo_map_syncs(dev->kmod.dev);
               util_dynarray_foreach(&batch->jobs, void *, job) {
                  const uint32_t *header = *job;
                  PANVK_PERF_NOLOG(
                          "PANVKDBG failed batch job: status=%08x task=%08x "
                          "fault=%08x%08x type_index=%08x\n",
                          header[0], header[1], header[3], header[2], header[4]);
               }
               return result;
            }
         }
      }
   }

   if (getenv("PANVK_DUMP_HEAP") && batch->tiler.ctx_descs.cpu) {
      /* The dump reads GPU written memory, so the window has to be retired. */
      VkResult drained = panvk_kbase_drain(dev);
      if (drained != VK_SUCCESS)
         return drained;

      const uint32_t *tc = (const uint32_t *)batch->tiler.ctx_descs.cpu;
      uint64_t poly = ((uint64_t)tc[1] << 32) | tc[0];
      uint64_t heap = ((uint64_t)tc[7] << 32) | tc[6];
      PANVK_PERF_NOLOG(
              "PANVKDBG tiler ctx: poly_list=%016llx heap_desc=%016llx (tc[2]=%08x)\n",
              (unsigned long long)poly, (unsigned long long)heap, tc[2]);
      if (dev->tiler_heap && dev->tiler_heap->addr.host) {
         uint64_t base = dev->tiler_heap->addr.dev;
         uint64_t size = pan_kmod_bo_size(dev->tiler_heap->bo);
         PANVK_PERF_NOLOG(
                 "PANVKDBG heap: base=%016llx size=%llx poly_off=%llx\n",
                 (unsigned long long)base, (unsigned long long)size,
                 (unsigned long long)(poly - base));
         if (batch->tiler.heap_desc.cpu) {
            const uint32_t *hd = (const uint32_t *)batch->tiler.heap_desc.cpu;
            PANVK_PERF_NOLOG(
                    "PANVKDBG heap_desc: w0=%08x w1=%08x w2=%08x w3=%08x w4=%08x w5=%08x w6=%08x w7=%08x\n",
                    hd[0], hd[1], hd[2], hd[3], hd[4], hd[5], hd[6], hd[7]);
         }
         unsigned char *h = dev->tiler_heap->addr.host;
         uint64_t masked = poly & 0x0000ffffffffffffull;
         if (masked >= base && (masked - base) < size) {
            uint64_t page = (masked - base) & ~0xfffull;
            panvk_priv_bo_invalidate(dev->tiler_heap, page, 0x2000);
            for (unsigned i = 0; i < 0x2000; i += 16) {
               const uint32_t *w = (const uint32_t *)(h + page + i);
               if (!(w[0] | w[1] | w[2] | w[3]))
                  continue;
               PANVK_PERF_NOLOG( "PANVKDBG poly[%05llx] %08x %08x %08x %08x\n",
                       (unsigned long long)(page + i), w[0], w[1], w[2], w[3]);
            }
         } else {
            PANVK_PERF_NOLOG( "PANVKDBG poly masked=%016llx off=%lld out of range\n",
                    (unsigned long long)masked, (long long)(masked - base));
         }
      }
   }

   batch->issued = true;
   return VK_SUCCESS;
}

VkResult
panvk_per_arch(kbase_jm_submit)(struct vk_queue *vk_queue,
                                struct panvk_gpu_queue *queue,
                                struct panvk_device *dev,
                                struct vk_queue_submit *submit)
{
   uint64_t targets[PANVK_KBASE_SYNC_TARGET_COUNT] = {};

   static uint64_t jm_submit_samples;
   static uint64_t jm_submit_batches[9];
   unsigned jm_this_submit_batches = 0;

   PANVK_PERF_NOLOG( "PANVKDBG kbase submit: wait=%u signal=%u cmdbuf=%u\n",
           submit->wait_count, submit->signal_count,
           submit->command_buffer_count);

   if (panvk_kbase_pending.failed)
      return vk_queue_set_lost(vk_queue, "kbase JM job did not complete");

   /* On kbase there are no DRM syncobjs: resolve incoming semaphore waits on
    * the CPU before emitting the jobs.  A wait is also the one point where a
    * submit must know that the jobs handed over earlier really finished. */
   if (submit->wait_count) {
      VkResult result = panvk_kbase_drain(dev);
      if (result != VK_SUCCESS)
         return vk_queue_set_lost(vk_queue, "kbase JM semaphore drain failed");

      result = vk_sync_wait_many(&dev->vk, submit->wait_count, submit->waits,
                                 VK_SYNC_WAIT_COMPLETE, UINT64_MAX);
      if (result != VK_SUCCESS)
         return result;
   }

   pan_kmod_flush_bo_map_syncs(dev->kmod.dev);

   const bool async = panvk_kbase_async_enabled();

   struct base_jd_atom_v2 emitted[2];
   unsigned emitted_count = 0;

   for (uint32_t j = 0; j < submit->command_buffer_count; ++j) {
      struct panvk_cmd_buffer *cmdbuf =
         container_of(submit->command_buffers[j], struct panvk_cmd_buffer, vk);

      unsigned nb = 0;
      list_for_each_entry(struct panvk_batch, batch, &cmdbuf->batches, node)
         nb++;
      PANVK_PERF_NOLOG( "PANVKDBG kbase submit cmdbuf[%u]: batches=%u\n", j, nb);

      /* Re-submitting a command buffer whose jobs are still running would
       * rewrite the descriptor pool the GPU is reading. */
      if (async && panvk_kbase_pending.count &&
          cmdbuf == panvk_kbase_pending.cmdbuf) {
         VkResult result = panvk_kbase_drain(dev);
         if (result != VK_SUCCESS)
            return vk_queue_set_lost(vk_queue,
                                     "kbase JM pool reuse drain failed");
      }

      list_for_each_entry(struct panvk_batch, batch, &cmdbuf->batches, node) {
         jm_this_submit_batches++;

         /* Bound the window.  The drain also recycles the atom ids, which
          * kbase only allows once the previous atom is no longer tracked. */
         if (async && (panvk_kbase_pending.batches >=
                          PANVK_KBASE_ASYNC_BATCHES ||
                       panvk_kbase_pending.count + 2 >
                          PANVK_KBASE_ASYNC_ATOMS)) {
            VkResult result = panvk_kbase_drain(dev);
            if (result != VK_SUCCESS)
               return vk_queue_set_lost(vk_queue,
                                        "kbase JM window drain failed");
         }

         VkResult result = panvk_kbase_jm_submit_batch(
            queue, cmdbuf, batch, NULL, 0, NULL, 0,
            async, /* pipeline */
            async ? &panvk_kbase_pending.next_id : NULL,
            panvk_kbase_pending.next_id - 1, emitted, &emitted_count);
         if (result != VK_SUCCESS)
            return vk_queue_set_lost(vk_queue, "kbase JM submission failed");

         if (async && emitted_count) {
            panvk_kbase_pending_add(dev, cmdbuf, emitted, emitted_count);
            panvk_kbase_pending.batches++;
         }
      }
   }

   if (getenv("PANVK_JM_STATS")) {
      unsigned bucket = MIN2(jm_this_submit_batches, 8u);
      jm_submit_batches[bucket]++;
      jm_submit_samples++;

      if (jm_submit_samples == 200) {
         fprintf(stderr,
                 "JM_SUBMIT_DIST 0=%llu 1=%llu 2=%llu 3=%llu 4=%llu "
                 "5=%llu 6=%llu 7=%llu 8+=%llu\\n",
                 (unsigned long long)jm_submit_batches[0],
                 (unsigned long long)jm_submit_batches[1],
                 (unsigned long long)jm_submit_batches[2],
                 (unsigned long long)jm_submit_batches[3],
                 (unsigned long long)jm_submit_batches[4],
                 (unsigned long long)jm_submit_batches[5],
                 (unsigned long long)jm_submit_batches[6],
                 (unsigned long long)jm_submit_batches[7],
                 (unsigned long long)jm_submit_batches[8]);

         jm_submit_samples = 0;
         memset(jm_submit_batches, 0, sizeof(jm_submit_batches));
      }
   }

   /* Jobs stay in flight after the submit returns; the CPU syncs are armed on
    * the wait hook, which drains the window so a fence is only ever observed
    * after the GPU work it covers has completed. */
   for (unsigned i = 0; i < submit->signal_count; i++) {
      assert(submit->signals[i].signal_value == 0);
      panvk_kbase_sync_set_pending(submit->signals[i].sync, queue,
                                   panvk_jm_kbase_wait_done, targets);
   }

   return VK_SUCCESS;
}
