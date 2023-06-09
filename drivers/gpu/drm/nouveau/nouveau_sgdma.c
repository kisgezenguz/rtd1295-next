// SPDX-License-Identifier: MIT
#include <linux/pagemap.h>
#include <linux/slab.h>

#include "nouveau_drv.h"
#include "nouveau_mem.h"
#include "nouveau_ttm.h"

struct nouveau_sgdma_be {
	/* this has to be the first field so populate/unpopulated in
	 * nouve_bo.c works properly, otherwise have to move them here
	 */
	struct ttm_dma_tt ttm;
	struct nouveau_mem *mem;
};

void
nouveau_sgdma_destroy(struct ttm_bo_device *bdev, struct ttm_tt *ttm)
{
	struct nouveau_sgdma_be *nvbe = (struct nouveau_sgdma_be *)ttm;

	if (ttm) {
		ttm_dma_tt_fini(&nvbe->ttm);
		kfree(nvbe);
	}
}

int
nouveau_sgdma_bind(struct ttm_bo_device *bdev, struct ttm_tt *ttm, struct ttm_resource *reg)
{
	struct nouveau_sgdma_be *nvbe = (struct nouveau_sgdma_be *)ttm;
	struct nouveau_drm *drm = nouveau_bdev(bdev);
	struct nouveau_mem *mem = nouveau_mem(reg);
	int ret;

	ret = nouveau_mem_host(reg, &nvbe->ttm);
	if (ret)
		return ret;

	if (drm->client.device.info.family < NV_DEVICE_INFO_V0_TESLA) {
		ret = nouveau_mem_map(mem, &mem->cli->vmm.vmm, &mem->vma[0]);
		if (ret) {
			nouveau_mem_fini(mem);
			return ret;
		}
	}

	nvbe->mem = mem;
	return 0;
}

void
nouveau_sgdma_unbind(struct ttm_bo_device *bdev, struct ttm_tt *ttm)
{
	struct nouveau_sgdma_be *nvbe = (struct nouveau_sgdma_be *)ttm;
	nouveau_mem_fini(nvbe->mem);
}

struct ttm_tt *
nouveau_sgdma_create_ttm(struct ttm_buffer_object *bo, uint32_t page_flags)
{
	struct nouveau_sgdma_be *nvbe;

	nvbe = kzalloc(sizeof(*nvbe), GFP_KERNEL);
	if (!nvbe)
		return NULL;

	if (ttm_dma_tt_init(&nvbe->ttm, bo, page_flags)) {
		kfree(nvbe);
		return NULL;
	}
	return &nvbe->ttm.ttm;
}
