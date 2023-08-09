/* SPDX-License-Identifier: GPL-2.0 or MIT */
/* Copyright 2019 Linaro, Ltd, Rob Herring <robh@kernel.org> */
/* Copyright 2023 Collabora ltd. */

#ifndef __PANTHOR_GEM_H__
#define __PANTHOR_GEM_H__

#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_mm.h>

#include <linux/rwsem.h>

struct panthor_vm;

/**
 * struct panthor_gem_object - Driver specific GEM object.
 */
struct panthor_gem_object {
	/** @base: Inherit from drm_gem_shmem_object. */
	struct drm_gem_shmem_object base;

	/**
	 * @va_node: VA space allocated to this GEM.
	 *
	 * Should be NULL for all GEM objects managed by userspace.
	 *
	 * Not NULL when %PANTHOR_GEM_ALLOC_VA is passed as an address, in
	 * which case the GEM logic will auto-allocate a VA range before mapping
	 * to the VM.
	 *
	 * @exclusive_vm must be != NULL.
	 */
	struct drm_mm_node *va_node;

	/**
	 * @exclusive_vm: Exclusive VM this GEM object can be mapped to.
	 *
	 * If @exclusive_vm != NULL, any attempt to bind the GEM to a different
	 * VM will fail.
	 *
	 * All FW memory objects have this field set to the MCU VM.
	 */
	struct panthor_vm *exclusive_vm;

	/**
	 * @gpuva_list_lock: Custom GPUVA lock.
	 *
	 * Used to protect insertion of drm_gpuva elements to the
	 * drm_gem_object.gpuva.list list.
	 *
	 * We can't use the GEM resv for that, because drm_gpuva_link() is
	 * called in a dma-signaling path, where we're not allowed to take
	 * resv locks.
	 */
	struct mutex gpuva_list_lock;

	/** @flags: Combination of drm_panthor_bo_flags flags. */
	u32 flags;
};

static inline
struct panthor_gem_object *to_panthor_bo(struct drm_gem_object *obj)
{
	return container_of(to_drm_gem_shmem_obj(obj), struct panthor_gem_object, base);
}

struct drm_gem_object *panthor_gem_create_object(struct drm_device *ddev, size_t size);

struct drm_gem_object *
panthor_gem_prime_import_sg_table(struct drm_device *ddev,
				  struct dma_buf_attachment *attach,
				  struct sg_table *sgt);

int
panthor_gem_create_with_handle(struct drm_file *file,
			       struct drm_device *ddev,
			       struct panthor_vm *exclusive_vm,
			       size_t size,
			       u32 flags,
			       uint32_t *handle);

void panthor_gem_unmap_and_put(struct panthor_vm *vm, struct panthor_gem_object *bo,
			       u64 gpu_va, void *cpu_va);

/*
 * PANTHOR_GEM_ALLOC_VA: Use this magic address when you want the GEM
 * logic to auto-allocate the virtual address in the reserved kernel VA range.
 */
#define PANTHOR_GEM_ALLOC_VA		~0ull

struct panthor_gem_object *
panthor_gem_create_and_map(struct panthor_device *ptdev, struct panthor_vm *vm,
			   size_t size, u32 bo_flags, u32 vm_map_flags,
			   u64 *gpu_va, void **cpu_va);

#endif /* __PANTHOR_GEM_H__ */
