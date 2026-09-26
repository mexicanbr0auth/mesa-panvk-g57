/*
 * PanVK JM backend submit via raw kbase ioctls: prototypes.
 *
 * A implementacao vive em panvk_vX_gpu_queue_kbase.c, uma TU separada para
 * que o helper de submissao de batches possa ficar ao lado da versao DRM.
 */

#ifndef panvk_vX_gpu_queue_kbase_h
#define panvk_vX_gpu_queue_kbase_h

#include "panvk_device.h"
#include "panvk_queue.h"

#ifdef HAVE_PAN_KMOD_KBASE
VkResult
panvk_per_arch(kbase_jm_submit)(struct vk_queue *vk_queue,
                                struct panvk_gpu_queue *queue,
                                struct panvk_device *dev,
                                struct vk_queue_submit *submit);
#endif

#if defined(HAVE_PAN_KMOD_KBASE) && defined(PANVK_USE_KBASE)
/* Espera pelos jobs JM ainda em voo.  Chamado antes de resetar ou destruir um
 * command buffer, para que os pools e as job chains que a GPU ainda pode estar
 * lendo nao sejam reciclados. */
void
panvk_per_arch(kbase_jm_drain)(struct panvk_device *dev);
#endif

#endif /* panvk_vX_gpu_queue_kbase_h */
