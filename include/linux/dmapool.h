/*
 * include/linux/dmapool.h
 *
 * Allocation pools for DMAable (coherent) memory.
 *
 * This file is licensed under  the terms of the GNU General Public 
 * License version 2. This program is licensed "as is" without any 
 * warranty of any kind, whether express or implied.
 */

#ifndef LINUX_DMAPOOL_H
#define	LINUX_DMAPOOL_H

#include <linux/nodemask_types.h>
#include <linux/scatterlist.h>
#include <asm/io.h>

struct device;

#ifdef CONFIG_HAS_DMA

struct dma_pool *dma_pool_create_node(const char *name, struct device *dev,
		size_t size, size_t align, size_t boundary, int node);

void dma_pool_destroy(struct dma_pool *pool);

void *dma_pool_alloc(struct dma_pool *pool, gfp_t mem_flags,
		     dma_addr_t *handle);
void dma_pool_free(struct dma_pool *pool, void *vaddr, dma_addr_t addr);

/*
 * Managed DMA pool
 */
struct dma_pool *dmam_pool_create(const char *name, struct device *dev,
				  size_t size, size_t align, size_t allocation);
void dmam_pool_destroy(struct dma_pool *pool);

#else /* !CONFIG_HAS_DMA */
static inline struct dma_pool *dma_pool_create_node(const char *name,
		struct device *dev, size_t size, size_t align, size_t boundary,
		int node)
{
	return NULL;
}
static inline void dma_pool_destroy(struct dma_pool *pool) { }
static inline void *dma_pool_alloc(struct dma_pool *pool, gfp_t mem_flags,
				   dma_addr_t *handle) { return NULL; }
static inline void dma_pool_free(struct dma_pool *pool, void *vaddr,
				 dma_addr_t addr) { }
static inline struct dma_pool *dmam_pool_create(const char *name,
	struct device *dev, size_t size, size_t align, size_t allocation)
{ return NULL; }
static inline void dmam_pool_destroy(struct dma_pool *pool) { }
#endif /* !CONFIG_HAS_DMA */

static inline struct dma_pool *dma_pool_create(const char *name,
		struct device *dev, size_t size, size_t align, size_t boundary)
{
	return dma_pool_create_node(name, dev, size, align, boundary,
				    NUMA_NO_NODE);
}

/**
 * dma_pool_zalloc - Get a zero-initialized block of DMA coherent memory.
 * @pool: dma pool that will produce the block
 * @mem_flags: GFP_* bitmask
 * @handle: pointer to dma address of block
 *
 * Same as dma_pool_alloc(), but the returned memory is zeroed.
 */
static inline void *dma_pool_zalloc(struct dma_pool *pool, gfp_t mem_flags,
				    dma_addr_t *handle)
{
	return dma_pool_alloc(pool, mem_flags | __GFP_ZERO, handle);
}

#endif

