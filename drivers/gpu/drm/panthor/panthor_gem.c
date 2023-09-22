// SPDX-License-Identifier: GPL-2.0 or MIT
/* Copyright 2019 Linaro, Ltd, Rob Herring <robh@kernel.org> */
/* Copyright 2023 Collabora ltd. */

#include <linux/err.h>
#include <linux/slab.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>

#include <drm/panthor_drm.h>

#include "panthor_device.h"
#include "panthor_gem.h"
#include "panthor_mmu.h"

static void panthor_gem_free_object(struct drm_gem_object *obj)
{
	struct panthor_gem_object *bo = to_panthor_bo(obj);

	if (drm_WARN_ON(obj->dev, bo->va_node))
		panthor_vm_free_va(bo->exclusive_vm, bo->va_node);

	if (bo->exclusive_vm) {
		panthor_vm_put(bo->exclusive_vm);
		bo->exclusive_vm = NULL;
		bo->base.base.resv = &bo->base.base._resv;
	}

	drm_gem_free_mmap_offset(&bo->base.base);
	mutex_destroy(&bo->gpuva_list_lock);
	drm_gem_shmem_free(&bo->base);
}

/**
 * panthor_gem_unmap_and_put() - Unmap and drop the reference on a GEM object
 * @vm: VM to unmap the GEM from.
 * @bo: GEM object to unmap/release.
 * @gpu_va: GPU/MCU virtual address the GEM object was mapped at.
 * @cpu_va: kernel mapping of the GEM object.
 * Can be NULL if the GEM was not CPU mapped.
 *
 * Should be called to undo what was done in panthor_gem_create_and_map().
 */
void panthor_gem_unmap_and_put(struct panthor_vm *vm,
			       struct panthor_gem_object *bo,
			       u64 gpu_va, void *cpu_va)
{
	if (cpu_va) {
		struct iosys_map map = IOSYS_MAP_INIT_VADDR(cpu_va);

		drm_gem_vunmap_unlocked(&bo->base.base, &map);
	}

	drm_WARN_ON(bo->base.base.dev, panthor_vm_unmap_range(vm, gpu_va, bo->base.base.size));
	panthor_vm_free_va(vm, bo->va_node);
	bo->va_node = NULL;
	drm_gem_object_put(&bo->base.base);
}

/**
 * panthor_gem_create_and_map() - Create and map a GEM object to a VM
 * @ptdev: Device.
 * @vm: VM to map the GEM to.
 * @bo_flags: Combination of drm_panthor_bo_flags flags.
 * @vm_map_flags: Combination of drm_panthor_vm_bind_op_flags (only those
 * that are related to map operations).
 * @gpu_va: Pointer holding the GPU address assigned when mapping to the VM.
 * If *gpu_va == PANTHOR_GEM_ALLOC_VA, a virtual address range will be allocated
 * and the allocated address returned, otherwise *gpu_va is used directly.
 * @cpu_va: Pointer holding the kernel CPU mapping. If NULL, the GEM object
 * is not CPU-mapped.
 *
 * Return: A valid pointer in case of success, an ERR_PTR() otherwise.
 */
struct panthor_gem_object *
panthor_gem_create_and_map(struct panthor_device *ptdev, struct panthor_vm *vm,
			   size_t size, u32 bo_flags, u32 vm_map_flags,
			   u64 *gpu_va, void **cpu_va)
{
	struct drm_gem_shmem_object *obj;
	struct panthor_gem_object *bo;
	int ret;

	obj = drm_gem_shmem_create(&ptdev->base, size);
	if (!obj)
		return ERR_PTR(-ENOMEM);

	bo = to_panthor_bo(&obj->base);
	bo->flags = bo_flags;
	bo->exclusive_vm = panthor_vm_get(vm);
	bo->base.base.resv = panthor_vm_resv(vm);

	if (*gpu_va == PANTHOR_GEM_ALLOC_VA) {
		bo->va_node = panthor_vm_alloc_va(vm, obj->base.size);

		if (IS_ERR(bo->va_node)) {
			ret = PTR_ERR(bo->va_node);
			bo->va_node = NULL;
			goto err_put_obj;
		}

		*gpu_va = bo->va_node->start;
	}

	ret = panthor_vm_map_bo_range(vm, bo, 0, obj->base.size, *gpu_va, vm_map_flags);
	if (ret)
		goto err_put_obj;

	if (cpu_va) {
		struct iosys_map map;
		int ret;

		ret = drm_gem_vmap_unlocked(&obj->base, &map);
		if (ret)
			goto err_vm_unmap_range;

		*cpu_va = map.vaddr;
	}

	return bo;

err_vm_unmap_range:
	panthor_vm_unmap_range(vm, *gpu_va, obj->base.size);

err_put_obj:
	drm_gem_object_put(&obj->base);
	return ERR_PTR(ret);
}

static int panthor_gem_mmap(struct drm_gem_object *obj, struct vm_area_struct *vma)
{
	struct panthor_gem_object *bo = to_panthor_bo(obj);

	/* Don't allow mmap on objects that have the NO_MMAP flag set. */
	if (bo->flags & DRM_PANTHOR_BO_NO_MMAP)
		return -EINVAL;

	return drm_gem_shmem_object_mmap(obj, vma);
}

static struct dma_buf *
panthor_gem_prime_export(struct drm_gem_object *obj, int flags)
{
	/* We can't export GEMs that have an exclusive VM. */
	if (to_panthor_bo(obj)->exclusive_vm)
		return ERR_PTR(-EINVAL);

	return drm_gem_prime_export(obj, flags);
}

static const struct drm_gem_object_funcs panthor_gem_funcs = {
	.free = panthor_gem_free_object,
	.print_info = drm_gem_shmem_object_print_info,
	.pin = drm_gem_shmem_object_pin,
	.unpin = drm_gem_shmem_object_unpin,
	.get_sg_table = drm_gem_shmem_object_get_sg_table,
	.vmap = drm_gem_shmem_object_vmap,
	.vunmap = drm_gem_shmem_object_vunmap,
	.mmap = panthor_gem_mmap,
	.export = panthor_gem_prime_export,
	.vm_ops = &drm_gem_shmem_vm_ops,
};

/**
 * panthor_gem_create_object - Implementation of driver->gem_create_object.
 * @dev: DRM device
 * @size: Size in bytes of the memory the object will reference
 *
 * This lets the GEM helpers allocate object structs for us, and keep
 * our BO stats correct.
 */
struct drm_gem_object *panthor_gem_create_object(struct drm_device *ddev, size_t size)
{
	struct panthor_device *ptdev = container_of(ddev, struct panthor_device, base);
	struct panthor_gem_object *obj;

	obj = kzalloc(sizeof(*obj), GFP_KERNEL);
	if (!obj)
		return ERR_PTR(-ENOMEM);

	obj->base.base.funcs = &panthor_gem_funcs;
	obj->base.map_wc = !ptdev->coherent;
	mutex_init(&obj->gpuva_list_lock);
	drm_gem_gpuva_set_lock(&obj->base.base, &obj->gpuva_list_lock);

	return &obj->base.base;
}

/**
 * panthor_gem_create_with_handle() - Create a GEM object and attach it to a handle.
 * @file: DRM file.
 * @ddev: DRM device.
 * @exclusive_vm: Exclusive VM. Not NULL if the GEM object can't be shared.
 * @size: Size of the GEM object to allocate.
 * @flags: Combination of drm_panthor_bo_flags flags.
 * @handle: Pointer holding the handle pointing to the new GEM object.
 *
 * Return: Zero on success
 */
int
panthor_gem_create_with_handle(struct drm_file *file,
			       struct drm_device *ddev,
			       struct panthor_vm *exclusive_vm,
			       size_t size,
			       u32 flags, u32 *handle)
{
	int ret;
	struct drm_gem_shmem_object *shmem;
	struct panthor_gem_object *bo;

	shmem = drm_gem_shmem_create(ddev, size);
	if (IS_ERR(shmem))
		return PTR_ERR(shmem);

	bo = to_panthor_bo(&shmem->base);
	bo->flags = flags;

	if (exclusive_vm) {
		bo->exclusive_vm = panthor_vm_get(exclusive_vm);
		bo->base.base.resv = panthor_vm_resv(exclusive_vm);
	}

	/*
	 * Allocate an id of idr table where the obj is registered
	 * and handle has the id what user can see.
	 */
	ret = drm_gem_handle_create(file, &shmem->base, handle);
	/* drop reference from allocate - handle holds it now. */
	drm_gem_object_put(&shmem->base);

	return ret;
}
